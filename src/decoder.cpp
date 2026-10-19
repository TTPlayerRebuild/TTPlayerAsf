#include "common.h"
#include <mediaobj.h>
#include <wmcodecdsp.h>
#include <uuids.h>
#include <ocidl.h>
#include <dmoreg.h>
#include <ks.h>
#include <ksmedia.h>
namespace ttp::asf {
namespace {
class MediaBuffer final:public IMediaBuffer{
 LONG refs_{1};Bytes data_;DWORD length_{};public:
 explicit MediaBuffer(DWORD capacity):data_(capacity){}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void**p)override{if(!p)return E_POINTER;*p=nullptr;if(!same(id,IID_IUnknown)&&!same(id,__uuidof(IMediaBuffer)))return E_NOINTERFACE;*p=this;AddRef();return S_OK;}
 ULONG STDMETHODCALLTYPE AddRef()override{return InterlockedIncrement(&refs_);}ULONG STDMETHODCALLTYPE Release()override{auto n=InterlockedDecrement(&refs_);if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE SetLength(DWORD n)override{if(n>data_.size())return E_INVALIDARG;length_=n;return S_OK;}
 HRESULT STDMETHODCALLTYPE GetMaxLength(DWORD*n)override{if(!n)return E_POINTER;*n=DWORD(data_.size());return S_OK;}
 HRESULT STDMETHODCALLTYPE GetBufferAndLength(BYTE**p,DWORD*n)override{if(p)*p=data_.data();if(n)*n=length_;return S_OK;}
};
void free_type(DMO_MEDIA_TYPE&t){CoTaskMemFree(t.pbFormat);if(t.pUnk)t.pUnk->Release();t={};}
GUID subtype(const WAVEFORMATEX*f){if(f->wFormatTag==WAVE_FORMAT_EXTENSIBLE&&f->cbSize>=22)return reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat;return GUID{f->wFormatTag,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};}
bool pcm_format(const WAVEFORMATEX*f,DWORD bytes){if(bytes<sizeof(WAVEFORMATEX)||sizeof(WAVEFORMATEX)+f->cbSize>bytes||!f->nChannels||!f->nSamplesPerSec||!f->nBlockAlign)return false;auto type=subtype(f);return same(type,KSDATAFORMAT_SUBTYPE_PCM)?f->wBitsPerSample==8||f->wBitsPerSample==16||f->wBitsPerSample==24||f->wBitsPerSample==32:same(type,KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)&&(f->wBitsPerSample==32||f->wBitsPerSample==64);}
HRESULT configure(IMediaObject*object,const WAVEFORMATEX*in,WAVEFORMATEX*out,DWORD capacity){return protect([&]{
 DMO_MEDIA_TYPE input{};input.majortype=MEDIATYPE_Audio;input.subtype=subtype(in);input.bFixedSizeSamples=TRUE;input.lSampleSize=in->nBlockAlign;input.formattype=FORMAT_WaveFormatEx;input.cbFormat=sizeof(WAVEFORMATEX)+in->cbSize;input.pbFormat=reinterpret_cast<BYTE*>(const_cast<WAVEFORMATEX*>(in));check(object->SetInputType(0,&input,0));
 ComPtr<IPropertyBag> bag;if(SUCCEEDED(object->QueryInterface(IID_IPropertyBag,reinterpret_cast<void**>(bag.put())))){VARIANT high{};high.vt=VT_BOOL;high.boolVal=VARIANT_TRUE;bag->Write(L"_HIRESOUTPUT",&high);}
 DMO_MEDIA_TYPE first{};if(SUCCEEDED(object->GetOutputType(0,0,&first))){bool valid=first.pbFormat&&first.cbFormat<=capacity&&pcm_format(reinterpret_cast<WAVEFORMATEX*>(first.pbFormat),first.cbFormat);HRESULT hr=valid?object->SetOutputType(0,&first,0):E_INVALIDARG;if(SUCCEEDED(hr))memcpy(out,first.pbFormat,first.cbFormat);free_type(first);if(SUCCEEDED(hr))return S_OK;}
 WORD bits=in->wBitsPerSample;if(bits!=8&&bits!=16&&bits!=24&&bits!=32)bits=16;WORD requested=out->wFormatTag;
 struct Trial{WORD tag,bits;};Trial trials[]={{requested==WAVE_FORMAT_IEEE_FLOAT?WORD(WAVE_FORMAT_IEEE_FLOAT):WORD(WAVE_FORMAT_PCM),requested==WAVE_FORMAT_IEEE_FLOAT?WORD(64):bits},{WAVE_FORMAT_PCM,bits},{WAVE_FORMAT_PCM,16}};
 HRESULT last=E_NOTIMPL;for(auto trial:trials){WAVEFORMATEXTENSIBLE wave{};wave.Format.wFormatTag=WAVE_FORMAT_EXTENSIBLE;wave.Format.nChannels=in->nChannels;wave.Format.nSamplesPerSec=in->nSamplesPerSec;wave.Format.wBitsPerSample=trial.bits;wave.Format.nBlockAlign=WORD(in->nChannels*trial.bits/8);wave.Format.nAvgBytesPerSec=in->nSamplesPerSec*wave.Format.nBlockAlign;wave.Format.cbSize=22;wave.Samples.wValidBitsPerSample=trial.bits;wave.dwChannelMask=in->wFormatTag==WAVE_FORMAT_EXTENSIBLE&&in->cbSize>=22?reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(in)->dwChannelMask:(in->nChannels==1?4:in->nChannels==2?3:in->nChannels==6?0x3f:0);wave.SubFormat=trial.tag==WAVE_FORMAT_PCM?KSDATAFORMAT_SUBTYPE_PCM:KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
  for(bool extended:{true,false}){DWORD bytes=extended?sizeof(wave):sizeof(WAVEFORMATEX);if(bytes>capacity)continue;if(!extended){wave.Format.wFormatTag=trial.tag;wave.Format.cbSize=0;}DMO_MEDIA_TYPE type{};type.majortype=MEDIATYPE_Audio;type.subtype=wave.SubFormat;type.bFixedSizeSamples=TRUE;type.lSampleSize=wave.Format.nBlockAlign;type.formattype=FORMAT_WaveFormatEx;type.cbFormat=bytes;type.pbFormat=reinterpret_cast<BYTE*>(&wave);last=object->SetOutputType(0,&type,0);if(SUCCEEDED(last)){memcpy(out,&wave,bytes);return S_OK;}}
 }return last;
});}
class DmoDecoder final:public Decoder{
 LONG refs_{1};HMODULE library_{};ComPtr<IMediaObject> object_;DWORD input_size_{},output_size_{},align_{};Bytes pcm_;size_t consumed_{};bool pending_{};
 ~DmoDecoder(){object_.reset();if(library_)FreeLibrary(library_);}
public:
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void**p)override{if(!p)return E_POINTER;*p=nullptr;if(!same(id,IID_IUnknown)&&!same(id,iid_decoder))return E_NOINTERFACE;*p=this;AddRef();return S_OK;}
 ULONG STDMETHODCALLTYPE AddRef()override{return InterlockedIncrement(&refs_);}ULONG STDMETHODCALLTYPE Release()override{auto n=InterlockedDecrement(&refs_);if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE Initialize(const WAVEFORMATEX*in,WAVEFORMATEX*out)override{return protect([&]{require(in&&out,E_POINTER);require(!object_,E_UNEXPECTED);require(in->nChannels&&in->nChannels<=32&&in->nSamplesPerSec&&in->cbSize<=65500,E_INVALIDARG);DWORD capacity=sizeof(WAVEFORMATEX)+out->cbSize;require(capacity<=65536,E_INVALIDARG);
   auto hr=protect([&]{library_=system_library(L"wmadmod.dll");auto get=reinterpret_cast<HRESULT(WINAPI*)(REFCLSID,REFIID,void**)>(GetProcAddress(library_,"DllGetClassObject"));require(get,E_NOINTERFACE);ComPtr<IClassFactory> factory;check(get(CLSID_CWMADecMediaObject,IID_IClassFactory,reinterpret_cast<void**>(factory.put())));check(factory->CreateInstance(nullptr,__uuidof(IMediaObject),reinterpret_cast<void**>(object_.put())));return configure(object_.p,in,out,capacity);});
   if(FAILED(hr)){object_.reset();HMODULE registry=system_library(L"msdmo.dll");using Enumerate=HRESULT(WINAPI*)(REFGUID,DWORD,DWORD,const DMO_PARTIAL_MEDIATYPE*,DWORD,const DMO_PARTIAL_MEDIATYPE*,IEnumDMO**);auto enumerate=reinterpret_cast<Enumerate>(GetProcAddress(registry,"DMOEnum"));ComPtr<IEnumDMO> list;DMO_PARTIAL_MEDIATYPE from{MEDIATYPE_Audio,subtype(in)},to{MEDIATYPE_Audio,KSDATAFORMAT_SUBTYPE_PCM};const GUID category{0x57f2db8b,0xe6bb,0x4513,{0x9d,0x43,0xdc,0xd2,0xa6,0x59,0x31,0x25}};hr=enumerate?enumerate(category,0,1,&from,1,&to,list.put()):E_NOTIMPL;
    if(SUCCEEDED(hr)){CLSID id{};wchar_t*name{};while(list->Next(1,&id,&name,nullptr)==S_OK){CoTaskMemFree(name);name=nullptr;ComPtr<IMediaObject> candidate;hr=CoCreateInstance(id,nullptr,CLSCTX_INPROC_SERVER,__uuidof(IMediaObject),reinterpret_cast<void**>(candidate.put()));if(SUCCEEDED(hr))hr=configure(candidate.p,in,out,capacity);if(SUCCEEDED(hr)){object_=std::move(candidate);break;}}}list.reset();FreeLibrary(registry);require(!!object_,FAILED(hr)?hr:E_NOTIMPL);
   }align_=out->nBlockAlign;
   DWORD lookahead{},alignment{};check(object_->GetInputSizeInfo(0,&input_size_,&lookahead,&alignment));check(object_->GetOutputSizeInfo(0,&output_size_,&alignment));require(output_size_&&output_size_<=64*1024*1024,E_INVALIDARG);
   // Original 601557A9 explicitly allocates before the host polls slot 5.
   // Lossless DMO otherwise reports !ACCEPT_DATA until the first ProcessInput,
   // which a correctly polling host will never call.
   object_->AllocateStreamingResources();return S_OK;});}
 HRESULT STDMETHODCALLTYPE BufferSizes(DWORD*in,DWORD*out)override{if(!object_)return E_UNEXPECTED;if(in)*in=input_size_;if(out)*out=output_size_;return S_OK;}
 HRESULT STDMETHODCALLTYPE NeedsInput()override{if(!object_)return E_UNEXPECTED;DWORD flags{};auto hr=object_->GetInputStatus(0,&flags);if(FAILED(hr))return hr;return flags&DMO_INPUT_STATUSF_ACCEPT_DATA?S_OK:S_FALSE;}
 HRESULT STDMETHODCALLTYPE OutputAvailable()override{return object_?(pending_||consumed_<pcm_.size()?S_OK:S_FALSE):E_UNEXPECTED;}
 HRESULT STDMETHODCALLTYPE Input(Buffer*buffer)override{return protect([&]{require(!!object_,E_UNEXPECTED);require(buffer,E_POINTER);BYTE*p{};DWORD n{},capacity{};check(buffer->Data(&p,&n));check(buffer->Capacity(&capacity));require(n<=capacity&&(!n||p),E_INVALIDARG);if(!n){check(object_->Discontinuity(0));pending_=true;return S_OK;}
   ComPtr<MediaBuffer> input(new MediaBuffer(n));BYTE*target{};input->GetBufferAndLength(&target,nullptr);memcpy(target,p,n);input->SetLength(n);check(object_->ProcessInput(0,input.p,0,0,0));pending_=true;return S_OK;});}
 HRESULT STDMETHODCALLTYPE Output(Buffer*buffer)override{return protect([&]{require(buffer,E_POINTER);check(buffer->SetLength(0));require(!!object_,E_UNEXPECTED);DWORD capacity{},unused{};BYTE*out{};check(buffer->Capacity(&capacity));check(buffer->Data(&out,&unused));capacity-=capacity%align_;require(capacity,E_INVALIDARG);require(out,E_POINTER);
   if(consumed_==pcm_.size()){
     ComPtr<MediaBuffer> data(new MediaBuffer(output_size_));DMO_OUTPUT_DATA_BUFFER result{};result.pBuffer=data.p;DWORD status{};auto hr=object_->ProcessOutput(0,1,&result,&status);check(hr);BYTE*p{};DWORD n{};data->GetBufferAndLength(&p,&n);require(n%align_==0,E_FAIL);if(!n){pending_=false;return S_OK;}pcm_.assign(p,p+n);consumed_=0;
   }
   DWORD n=DWORD(std::min<size_t>(capacity,pcm_.size()-consumed_));memcpy(out,pcm_.data()+consumed_,n);consumed_+=n;return buffer->SetLength(n);});}
 HRESULT STDMETHODCALLTYPE Reset()override{if(!object_)return E_UNEXPECTED;pcm_.clear();consumed_=0;pending_=false;return object_->Flush();}
};
}
HRESULT make_decoder(void**out){if(!out)return E_POINTER;*out=nullptr;return protect([&]{*out=static_cast<Decoder*>(new DmoDecoder);return S_OK;});}
}


