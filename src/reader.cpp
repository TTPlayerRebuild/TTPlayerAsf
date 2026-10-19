#include "io.h"
#include "native_asf.h"
#include <wmsdk.h>
#include <nserror.h>
#include <atomic>
#include <cwchar>
namespace ttp::asf {
namespace {
std::wstring attribute(const char*key){
 struct Map{const char*key;const wchar_t*name;};
 static const Map names[]={{"Title",L"Title"},{"Artist",L"Author"},{"Album",L"WM/AlbumTitle"},{"AlbumArtist",L"WM/AlbumArtist"},{"Date",L"WM/Year"},{"Year",L"WM/Year"},{"Genre",L"WM/Genre"},{"Comment",L"Description"},{"Tracknumber",L"WM/TrackNumber"},{"Track",L"WM/TrackNumber"},{"Composer",L"WM/Composer"},{"Copyright",L"Copyright"},{"Discnumber",L"WM/PartOfSet"},{"Publisher",L"WM/Publisher"},{"Lyrics",L"WM/Lyrics"}};
 for(auto&m:names)if(!_stricmp(key,m.key))return m.name;return wide(key);
}
std::wstring bounded_string(const wchar_t*p,const Bytes&storage){
 auto begin=reinterpret_cast<uintptr_t>(storage.data()),end=begin+storage.size(),at=reinterpret_cast<uintptr_t>(p);
 require(at>=begin&&at<end);size_t count=(end-at)/sizeof(wchar_t);
 // WM_PICTURE is packed(1); the SDK can place UTF-16 text at odd offsets.
 std::wstring value(count,L'\0');memcpy(value.data(),p,count*sizeof(wchar_t));auto n=value.find(L'\0');require(n!=std::wstring::npos);value.resize(n);return value;
}
struct Tag{std::wstring name,value;WMT_ATTR_DATATYPE type{WMT_TYPE_STRING};};
struct Cover{std::wstring mime,description;Bytes bytes;DWORD type{};Picture view{};};
// WM's synchronous reader expects ordinary IStream reads to fill the requested
// range. The player's network/archived streams may return short blocks and use
// their extra cancellable Read slot. Keep that contract at the SDK boundary.
class StreamBridge final:public IStream{
 LONG refs_{1};Input&input_;
public:
 explicit StreamBridge(Input&input):input_(input){}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void**out)override{if(!out)return E_POINTER;*out=nullptr;if(!same(id,IID_IUnknown)&&!same(id,IID_IStream)&&!same(id,IID_ISequentialStream))return E_NOINTERFACE;*out=this;AddRef();return S_OK;}
 ULONG STDMETHODCALLTYPE AddRef()override{return InterlockedIncrement(&refs_);}ULONG STDMETHODCALLTYPE Release()override{auto n=InterlockedDecrement(&refs_);if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE Read(void*p,ULONG n,ULONG*got)override{if(got)*got=0;return protect([&]{require(!n||p,E_POINTER);ULONG total{};while(total<n){require(WaitForSingleObject(input_.event,0)!=WAIT_OBJECT_0,HRESULT_FROM_WIN32(ERROR_CANCELLED));auto count=input_.read(static_cast<BYTE*>(p)+total,n-total);if(!count)break;total+=count;if(got)*got=total;}return total==n?S_OK:S_FALSE;});}
 HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move,DWORD origin,ULARGE_INTEGER*out)override{return protect([&]{require(input_.seekable,STG_E_INVALIDFUNCTION);uint64_t start{};if(origin==STREAM_SEEK_CUR)start=tell(input_.stream.p);else if(origin==STREAM_SEEK_END)start=input_.size;else if(origin==STREAM_SEEK_SET)start=input_.base;else return E_INVALIDARG;
  uint64_t amount=move.QuadPart<0?uint64_t(-(move.QuadPart+1))+1:uint64_t(move.QuadPart);require(move.QuadPart<0?amount<=start:amount<=INT64_MAX-start,STG_E_INVALIDFUNCTION);uint64_t target=move.QuadPart<0?start-amount:start+amount;require(target>=input_.base,STG_E_INVALIDFUNCTION);seek(input_.stream.p,target);if(out)out->QuadPart=target-input_.base;return S_OK;});}
 HRESULT STDMETHODCALLTYPE Stat(STATSTG*out,DWORD flags)override{if(!out)return E_POINTER;auto hr=input_.stream->Stat(out,flags);if(SUCCEEDED(hr))out->cbSize.QuadPart=out->cbSize.QuadPart>=input_.base?out->cbSize.QuadPart-input_.base:0;return hr;}
 HRESULT STDMETHODCALLTYPE Write(const void*,ULONG,ULONG*n)override{if(n)*n=0;return STG_E_ACCESSDENIED;}
 HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER)override{return STG_E_ACCESSDENIED;}
 HRESULT STDMETHODCALLTYPE CopyTo(IStream*,ULARGE_INTEGER,ULARGE_INTEGER*,ULARGE_INTEGER*)override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE Commit(DWORD)override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE Revert()override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER,ULARGE_INTEGER,DWORD)override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER,ULARGE_INTEGER,DWORD)override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE Clone(IStream**)override{return E_NOTIMPL;}
};
class AsfReader final:public Reader,public Metadata,public Thumbnail,public MetadataCommit{
 LONG refs_{1};Input input_;ComPtr<IStream> bridge_;HMODULE library_{};ComPtr<IWMSyncReader> reader_;Bytes format_,pending_;WORD stream_number_{};
 DWORD duration_{},bitrate_{},recommended_{4096};std::atomic_bool stopped_{};bool opened_{},ended_{},terminal_{};HRESULT commit_result_{S_OK};
 std::unique_ptr<NativeAsf> native_;bool native_writable_{};
 std::vector<Tag> tags_,changes_;std::vector<Cover> covers_;bool covers_changed_{};
 using CreateReader=HRESULT(WINAPI*)(IUnknown*,DWORD,IWMSyncReader**);
 using CreateEditor=HRESULT(WINAPI*)(IWMMetadataEditor**);
 void mutable_metadata(){require(opened_&&!terminal_,E_UNEXPECTED);require(!native_||native_writable_,STG_E_ACCESSDENIED);require(!input_.remote&&input_.seekable&&input_.base==0&&!input_.path.empty(),STG_E_ACCESSDENIED);DWORD attributes=GetFileAttributesW(input_.path.c_str());require(attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&(FILE_ATTRIBUTE_READONLY|FILE_ATTRIBUTE_DIRECTORY)),STG_E_ACCESSDENIED);
  // Named virtual/archive streams must never modify the container at that path.
  WIN32_FILE_ATTRIBUTE_DATA info{};require(GetFileAttributesExW(input_.path.c_str(),GetFileExInfoStandard,&info),STG_E_ACCESSDENIED);require((uint64_t(info.nFileSizeHigh)<<32|info.nFileSizeLow)==input_.size,STG_E_ACCESSDENIED);
 }
 void close_reader(){if(reader_){reader_->Close();reader_.reset();}bridge_.reset();input_.extended.reset();input_.stream.reset();}
 ~AsfReader(){if(!terminal_&&(!changes_.empty()||covers_changed_))Commit();close_reader();if(library_)FreeLibrary(library_);}
 void load_tags(IUnknown*source){
  ComPtr<IWMHeaderInfo3> header;check(source->QueryInterface(__uuidof(IWMHeaderInfo3),reinterpret_cast<void**>(header.put())));
  WORD count{};check(header->GetAttributeCountEx(0,&count));
  for(WORD i=0;i<count;++i){WORD chars{},language{};DWORD bytes{};WMT_ATTR_DATATYPE type{};
   check(header->GetAttributeByIndexEx(0,i,nullptr,&chars,&type,&language,nullptr,&bytes));require(chars>0&&chars<=32767&&bytes<=64*1024*1024);
   std::wstring name(chars,L'\0');Bytes data(bytes);check(header->GetAttributeByIndexEx(0,i,name.data(),&chars,&type,&language,data.data(),&bytes));name.resize(wcsnlen(name.c_str(),name.size()));
   if(name==L"Duration"&&type==WMT_TYPE_QWORD&&bytes==8){uint64_t ticks{};memcpy(&ticks,data.data(),8);duration_=dword(ticks/10000);}
   else if(name==L"Bitrate"&&type==WMT_TYPE_DWORD&&bytes==4)memcpy(&bitrate_,data.data(),4);
   else if(name==L"WM/Picture"&&type==WMT_TYPE_BINARY&&bytes>=sizeof(WM_PICTURE)){
    WM_PICTURE picture{};memcpy(&picture,data.data(),sizeof(picture));uintptr_t p=reinterpret_cast<uintptr_t>(picture.pbData),begin=reinterpret_cast<uintptr_t>(data.data());
    require(p>=begin&&p-begin<=data.size()&&picture.dwDataLen<=data.size()-(p-begin));Cover c;c.mime=bounded_string(picture.pwszMIMEType,data);c.description=bounded_string(picture.pwszDescription,data);c.type=picture.bPictureType;c.bytes.assign(picture.pbData,picture.pbData+picture.dwDataLen);covers_.push_back(std::move(c));
   }else if(type==WMT_TYPE_STRING&&bytes>=2&&bytes%2==0){auto*p=reinterpret_cast<const wchar_t*>(data.data());std::wstring value(p,wcsnlen(p,bytes/2));
    if(!value.empty()&&std::none_of(tags_.begin(),tags_.end(),[&](const Tag&t){return !_wcsicmp(t.name.c_str(),name.c_str());}))tags_.push_back({name,std::move(value),type});}
   else if(name==L"WM/TrackNumber"&&type==WMT_TYPE_DWORD&&bytes==4){DWORD n{};memcpy(&n,data.data(),4);tags_.push_back({name,std::to_wstring(n),type});}
  }
 }
 void native_tags(){
  tags_.clear();covers_.clear();
  for(const auto&a:native_->attributes){auto&data=a.value;
   if(a.type==0 && !(data.size()&1)){std::wstring value(data.size()/2,L'\0');if(!data.empty())memcpy(value.data(),data.data(),data.size());while(!value.empty()&&!value.back())value.pop_back();if(!value.empty())tags_.push_back({a.name,std::move(value),WMT_TYPE_STRING});}
   else if(a.type==3&&data.size()==4)tags_.push_back({a.name,std::to_wstring(le32(data.data())),WMT_TYPE_DWORD});
   else if(a.name==L"WM/Picture"&&a.type==1&&data.size()>=9){Cover c;c.type=data[0];DWORD n=le32(data.data()+1);size_t offset=5;auto string=[&]{std::wstring result;for(;;){require(offset+2<=data.size());auto ch=le16(data.data()+offset);offset+=2;if(!ch)return result;result.push_back(wchar_t(ch));}};c.mime=string();c.description=string();require(n<=data.size()-offset);c.bytes.assign(data.data()+offset,data.data()+offset+n);covers_.push_back(std::move(c));}
  }
 }
 static void erase(IWMHeaderInfo3*h,const std::wstring&name){WORD count{};auto hr=h->GetAttributeIndices(0,name.c_str(),nullptr,nullptr,&count);if(FAILED(hr)&&!count)return;if(!count)return;std::vector<WORD> indices(count);check(h->GetAttributeIndices(0,name.c_str(),nullptr,indices.data(),&count));std::sort(indices.rbegin(),indices.rend());for(auto i:indices)check(h->DeleteAttribute(0,i));}
 void save(){
  require(!input_.path.empty()&&!input_.remote,STG_E_ACCESSDENIED);
  auto create=reinterpret_cast<CreateEditor>(GetProcAddress(library_,"WMCreateEditor"));require(create,E_NOTIMPL);
  auto path=input_.path;auto directory=path.substr(0,path.find_last_of(L"\\/")+1);wchar_t temp[MAX_PATH]{};
  require(GetTempFileNameW(directory.c_str(),L"asf",0,temp)!=0,HRESULT_FROM_WIN32(GetLastError()));
  auto result=protect([&]{
   close_reader();require(CopyFileW(path.c_str(),temp,FALSE)!=0,HRESULT_FROM_WIN32(GetLastError()));
   ComPtr<IWMMetadataEditor> editor;check(create(editor.put()));check(editor->Open(temp));ComPtr<IWMHeaderInfo3> header;check(editor->QueryInterface(__uuidof(IWMHeaderInfo3),reinterpret_cast<void**>(header.put())));
   for(auto&change:changes_){erase(header.p,change.name);if(change.value.empty())continue;WORD index{};
    if(change.type==WMT_TYPE_DWORD){wchar_t*end{};unsigned long n=wcstoul(change.value.c_str(),&end,10);require(end&&!*end,E_INVALIDARG);DWORD value=n;check(header->AddAttribute(0,change.name.c_str(),&index,WMT_TYPE_DWORD,0,reinterpret_cast<BYTE*>(&value),4));}
    else{require(change.value.size()<=32767,E_INVALIDARG);check(header->AddAttribute(0,change.name.c_str(),&index,WMT_TYPE_STRING,0,reinterpret_cast<BYTE*>(change.value.data()),DWORD((change.value.size()+1)*2)));}
   }
   if(covers_changed_){erase(header.p,L"WM/Picture");for(auto&c:covers_){WM_PICTURE p{};p.pwszMIMEType=c.mime.data();p.pwszDescription=c.description.data();p.bPictureType=BYTE(c.type);p.pbData=c.bytes.data();p.dwDataLen=DWORD(c.bytes.size());WORD index{};check(header->AddAttribute(0,L"WM/Picture",&index,WMT_TYPE_BINARY,0,reinterpret_cast<BYTE*>(&p),sizeof(p)));}}
   header.reset();check(editor->Flush());check(editor->Close());editor.reset();require(MoveFileExW(temp,path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0,HRESULT_FROM_WIN32(GetLastError()));return S_OK;
  });if(FAILED(result)){DWORD attrs=GetFileAttributesW(temp);if(attrs!=INVALID_FILE_ATTRIBUTES&&(attrs&FILE_ATTRIBUTE_READONLY))SetFileAttributesW(temp,attrs&~FILE_ATTRIBUTE_READONLY);DeleteFileW(temp);}check(result);
 }
 Cover copy_picture(const Picture*p){require(p&&p->size>=sizeof(Picture),E_INVALIDARG);require(p->bytes&&p->bytes<=60000&&p->data&&p->type<=255,E_INVALIDARG);Cover c;c.mime=p->mime?p->mime:L"image/jpeg";c.description=p->description?p->description:L"";c.type=p->type;c.bytes.assign(p->data,p->data+p->bytes);return c;}
public:
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void**out)override{if(!out)return E_POINTER;*out=nullptr;if(same(id,IID_IUnknown)||same(id,iid_reader))*out=static_cast<Reader*>(this);else if(same(id,iid_metadata))*out=static_cast<Metadata*>(this);else if(same(id,iid_thumbnail))*out=static_cast<Thumbnail*>(this);else if(same(id,iid_metadata_commit))*out=static_cast<MetadataCommit*>(this);else return E_NOINTERFACE;AddRef();return S_OK;}
 ULONG STDMETHODCALLTYPE AddRef()override{return InterlockedIncrement(&refs_);}ULONG STDMETHODCALLTYPE Release()override{auto n=InterlockedDecrement(&refs_);if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE Open(IStream*stream,DWORD)override{return protect([&]{require(stream,E_POINTER);require(!opened_&&!reader_,E_UNEXPECTED);input_.open(stream);bridge_.reset(new StreamBridge(input_));library_=system_library(L"wmvcore.dll");auto create=reinterpret_cast<CreateReader>(GetProcAddress(library_,"WMCreateSyncReader"));require(create,E_NOTIMPL);check(create(nullptr,0,reader_.put()));auto open_result=reader_->OpenStream(bridge_.p);
  if(FAILED(open_result)) {
   reader_->Close();reader_.reset();bridge_.reset();native_=std::make_unique<NativeAsf>(input_);native_->open();format_=native_->format;duration_=native_->duration;bitrate_=native_->bitrate;recommended_=native_->buffer_size();
   // Header editing does not require the unavailable WM reader codec. Preserve
   // the same metadata contract when the system editor accepts this container.
   auto editor_create=reinterpret_cast<CreateEditor>(GetProcAddress(library_,"WMCreateEditor"));
   native_tags();
   WIN32_FILE_ATTRIBUTE_DATA file_info{};bool is_file=!input_.remote&&input_.base==0&&!input_.path.empty()&&GetFileAttributesExW(input_.path.c_str(),GetFileExInfoStandard,&file_info)&&(uint64_t(file_info.nFileSizeHigh)<<32|file_info.nFileSizeLow)==input_.size;
   if(editor_create&&is_file) {ComPtr<IWMMetadataEditor> editor;if(SUCCEEDED(editor_create(editor.put()))&&SUCCEEDED(editor->Open(input_.path.c_str()))) {tags_.clear();covers_.clear();auto result=protect([&]{load_tags(editor.p);return S_OK;});editor->Close();native_writable_=SUCCEEDED(result);if(!native_writable_)native_tags();}}
   opened_=true;return S_OK;
  }
  ComPtr<IWMProfile> profile;check(reader_->QueryInterface(__uuidof(IWMProfile),reinterpret_cast<void**>(profile.put())));DWORD count{};check(profile->GetStreamCount(&count));
  for(DWORD i=0;i<count;++i){ComPtr<IWMStreamConfig> config;check(profile->GetStream(i,config.put()));GUID type{};check(config->GetStreamType(&type));if(type!=WMMEDIATYPE_Audio)continue;ComPtr<IWMMediaProps> props;check(config->QueryInterface(__uuidof(IWMMediaProps),reinterpret_cast<void**>(props.put())));DWORD bytes{};check(props->GetMediaType(nullptr,&bytes));require(bytes>=sizeof(WM_MEDIA_TYPE)&&bytes<=1024*1024);Bytes data(bytes);auto media=reinterpret_cast<WM_MEDIA_TYPE*>(data.data());check(props->GetMediaType(media,&bytes));require(media->pbFormat&&media->cbFormat>=sizeof(WAVEFORMATEX)&&media->cbFormat<=65536);auto f=reinterpret_cast<const WAVEFORMATEX*>(media->pbFormat);require(f->nBlockAlign&&f->nChannels&&f->nSamplesPerSec,E_NOTIMPL);require(sizeof(WAVEFORMATEX)+f->cbSize<=media->cbFormat);format_.assign(media->pbFormat,media->pbFormat+sizeof(WAVEFORMATEX)+f->cbSize);check(config->GetStreamNumber(&stream_number_));break;}
  require(stream_number_!=0,E_NOTIMPL);check(reader_->SetReadStreamSamples(stream_number_,TRUE));check(reader_->GetMaxStreamSampleSize(stream_number_,&recommended_));require(recommended_&&recommended_<=64*1024*1024);load_tags(reader_.p);opened_=true;return S_OK;});}
 HRESULT STDMETHODCALLTYPE Capabilities(DWORD*out)override{if(!out)return E_POINTER;*out=input_.capabilities(false);if(native_&&!native_writable_)*out&=~4u;return S_OK;}
 HRESULT STDMETHODCALLTYPE Duration(DWORD*out)override{if(!out)return E_POINTER;*out=duration_;return opened_?S_OK:E_UNEXPECTED;}
 HRESULT STDMETHODCALLTYPE Format(WAVEFORMATEX**out)override{if(!out)return E_POINTER;*out=nullptr;if(!opened_)return E_UNEXPECTED;*out=static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(format_.size()));if(!*out)return E_OUTOFMEMORY;memcpy(*out,format_.data(),format_.size());return S_OK;}
 HRESULT STDMETHODCALLTYPE BufferSize(DWORD*out)override{if(!out)return E_POINTER;*out=recommended_;return opened_?S_OK:E_UNEXPECTED;}
 HRESULT STDMETHODCALLTYPE CodecName(wchar_t**out)override{return text(native_?L"MP3|MPEG Layer 3":L"WMA|Windows Media Audio",out);}
 HRESULT STDMETHODCALLTYPE Bitrate(DWORD*out)override{if(!out)return E_POINTER;*out=bitrate_;return S_OK;}
 HRESULT STDMETHODCALLTYPE BitDepth(WORD*out)override{if(!out)return E_POINTER;*out=format_.size()>=sizeof(WAVEFORMATEX)?reinterpret_cast<const WAVEFORMATEX*>(format_.data())->wBitsPerSample:0;return S_OK;}
 HRESULT STDMETHODCALLTYPE SetCallback(IUnknown*p)override{return input_.stream?input_.callback(p):E_UNEXPECTED;}
 HRESULT STDMETHODCALLTYPE Start()override{stopped_=false;ResetEvent(input_.event);return S_OK;}
 HRESULT STDMETHODCALLTYPE Stop()override{stopped_=true;SetEvent(input_.event);return S_OK;}
 HRESULT STDMETHODCALLTYPE Read(Buffer*buffer)override{return protect([&]{require(buffer,E_POINTER);check(buffer->SetLength(0));require((!!reader_||!!native_)&&!terminal_,E_UNEXPECTED);require(!stopped_,HRESULT_FROM_WIN32(ERROR_CANCELLED));DWORD capacity{},unused{};BYTE*out{};check(buffer->Capacity(&capacity));check(buffer->Data(&out,&unused));require(capacity,E_INVALIDARG);require(out,E_POINTER);
  if(native_&&pending_.empty()&&!ended_)ended_=!native_->read(pending_);
  for(unsigned i=0;!native_&&pending_.empty()&&!ended_&&i<10000;++i){ComPtr<INSSBuffer> sample;QWORD time{},duration{};DWORD flags{},output{};WORD number{};auto hr=reader_->GetNextSample(stream_number_,sample.put(),&time,&duration,&flags,&output,&number);if(hr==NS_E_NO_MORE_SAMPLES){ended_=true;break;}check(hr);BYTE*p{};DWORD n{};require(!!sample);check(sample->GetBufferAndLength(&p,&n));require(n<=64*1024*1024&&(!n||p));if(n)pending_.assign(p,p+n);}
  if(pending_.empty())return ended_?S_FALSE:E_FAIL;require(pending_.size()<=capacity,HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));memcpy(out,pending_.data(),pending_.size());DWORD n=DWORD(pending_.size());pending_.clear();return buffer->SetLength(n);});}
 HRESULT STDMETHODCALLTYPE Seek(DWORD*milliseconds)override{return protect([&]{require(milliseconds,E_POINTER);require((!!reader_||!!native_)&&!terminal_,E_UNEXPECTED);require(input_.seekable,STG_E_INVALIDFUNCTION);DWORD target=std::min(*milliseconds,duration_);pending_.clear();ended_=false;if(native_)native_->seek_to(target,pending_);else check(reader_->SetRange(uint64_t(target)*10000,0));*milliseconds=target;return S_OK;});}
 HRESULT STDMETHODCALLTYPE Count(DWORD*out)override{if(!out)return E_POINTER;*out=DWORD(tags_.size());return S_OK;}
 HRESULT STDMETHODCALLTYPE At(DWORD index,wchar_t**key,wchar_t**value)override{return protect([&]{require(key&&value,E_POINTER);*key=*value=nullptr;require(index<tags_.size(),E_INVALIDARG);check(text(tags_[index].name,key));auto hr=text(tags_[index].value,value);if(FAILED(hr)){CoTaskMemFree(*key);*key=nullptr;}return hr;});}
 HRESULT STDMETHODCALLTYPE Get(const char*key,wchar_t**out)override{return protect([&]{require(key&&out,E_POINTER);*out=nullptr;auto name=attribute(key);for(auto&t:tags_)if(!_wcsicmp(name.c_str(),t.name.c_str()))return text(t.value,out);return S_FALSE;});}
 HRESULT STDMETHODCALLTYPE Set(const char*key,const wchar_t*value)override{return protect([&]{require(key&&*key,E_INVALIDARG);mutable_metadata();auto name=attribute(key);Tag change{name,value?value:L"",WMT_TYPE_STRING};for(auto&t:tags_)if(!_wcsicmp(name.c_str(),t.name.c_str())){change.type=t.type;break;}if(name==L"WM/TrackNumber")change.type=WMT_TYPE_DWORD;
  tags_.erase(std::remove_if(tags_.begin(),tags_.end(),[&](const Tag&t){return !_wcsicmp(t.name.c_str(),name.c_str());}),tags_.end());if(!change.value.empty())tags_.push_back(change);
  for(auto&t:changes_)if(!_wcsicmp(t.name.c_str(),name.c_str())){t=std::move(change);return S_OK;}changes_.push_back(std::move(change));return S_OK;});}
 HRESULT STDMETHODCALLTYPE Commit()override{if(terminal_)return commit_result_;terminal_=true;commit_result_=protect([&]{if(!changes_.empty()||covers_changed_)save();return S_OK;});return commit_result_;}
 HRESULT STDMETHODCALLTYPE PictureCount(DWORD*out)override{if(!out)return E_POINTER;*out=DWORD(covers_.size());return S_OK;}
 HRESULT STDMETHODCALLTYPE MaximumBytes(DWORD*out)override{if(!out)return E_POINTER;*out=60000;return S_OK;}
 HRESULT STDMETHODCALLTYPE MaximumCount(DWORD*out)override{if(!out)return E_POINTER;*out=1;return S_OK;}
 HRESULT STDMETHODCALLTYPE PictureAt(DWORD index,const Picture**out)override{if(!out)return E_POINTER;*out=nullptr;if(index>=covers_.size())return E_INVALIDARG;auto&c=covers_[index];c.view={sizeof(Picture),c.mime.c_str(),c.description.c_str(),DWORD(c.bytes.size()),c.bytes.data(),c.type};*out=&c.view;return S_OK;}
 HRESULT STDMETHODCALLTYPE ReplacePicture(DWORD index,const Picture*p,DWORD mask)override{return protect([&]{mutable_metadata();require(p,E_POINTER);require(p->size>=sizeof(Picture)&&index<covers_.size()&&!(mask&~15u),E_INVALIDARG);auto c=covers_[index];if(mask&1)c.mime=p->mime?p->mime:L"";if(mask&2)c.description=p->description?p->description:L"";if(mask&4){require(p->type<=255,E_INVALIDARG);c.type=p->type;}if(mask&8){require(p->bytes&&p->bytes<=60000&&p->data,E_INVALIDARG);c.bytes.assign(p->data,p->data+p->bytes);}covers_[index]=std::move(c);covers_changed_=true;return S_OK;});}
 HRESULT STDMETHODCALLTYPE AddPicture(const Picture*p)override{return protect([&]{mutable_metadata();require(covers_.empty(),E_INVALIDARG);covers_.push_back(copy_picture(p));covers_changed_=true;return S_OK;});}
 HRESULT STDMETHODCALLTYPE RemovePicture(DWORD index)override{return protect([&]{mutable_metadata();require(index<covers_.size(),E_INVALIDARG);covers_.erase(covers_.begin()+index);covers_changed_=true;return S_OK;});}
};
}
HRESULT make_reader(void**out){if(!out)return E_POINTER;*out=nullptr;return protect([&]{*out=static_cast<Reader*>(new AsfReader);return S_OK;});}
}

