#pragma once
#include "io.h"
#include <deque>

namespace ttp::asf {
// The WM synchronous reader rejects some valid ASF audio types (notably MP3).
// The original 60157791/60158012 reader demultiplexes these packets itself.
// This independent, bounded implementation follows ASF 1.20.03 sections 3-5;
// codec decoding remains with the system DMO selected by the Decoder factory.
class NativeAsf {
    struct Cursor {
        const BYTE* data; size_t size, pos{};
        const BYTE* take(size_t n) { require(pos <= size && n <= size-pos); auto p=data+pos; pos+=n; return p; }
        BYTE byte() { return *take(1); }
        DWORD field(unsigned code) { unsigned n=code==3?4:code; auto p=take(n); return n==4?le32(p):n==2?le16(p):n==1?*p:0; }
    };
    static uint64_t u64(const BYTE* p) { return uint64_t(le32(p)) | (uint64_t(le32(p+4))<<32); }
    static bool guid(const BYTE* p,const GUID& id) { return !memcmp(p,&id,16); }
    static constexpr GUID header_id{0x75b22630,0x668e,0x11cf,{0xa6,0xd9,0,0xaa,0,0x62,0xce,0x6c}};
    static constexpr GUID data_id{0x75b22636,0x668e,0x11cf,{0xa6,0xd9,0,0xaa,0,0x62,0xce,0x6c}};
    static constexpr GUID file_id{0x8cabdca1,0xa947,0x11cf,{0x8e,0xe4,0,0xc0,0x0c,0x20,0x53,0x65}};
    static constexpr GUID stream_id{0xb7dc0791,0xa9b7,0x11cf,{0x8e,0xe6,0,0xc0,0x0c,0x20,0x53,0x65}};
    static constexpr GUID audio_id{0xf8699e40,0x5b4d,0x11cf,{0xa8,0xfd,0,0x80,0x5f,0x5c,0x44,0x2b}};
    static constexpr GUID no_ec_id{0x20fb5700,0x5b55,0x11cf,{0xa8,0xfd,0,0x80,0x5f,0x5c,0x44,0x2b}};
    static constexpr GUID spread_id{0xbfc3cd50,0x618f,0x11cf,{0x8b,0xb2,0,0xaa,0,0xb4,0xe2,0x20}};
    static constexpr GUID content_id{0x75b22633,0x668e,0x11cf,{0xa6,0xd9,0,0xaa,0,0x62,0xce,0x6c}};
    static constexpr GUID extended_id{0xd2d0a440,0xe307,0x11d2,{0x97,0xf0,0,0xa0,0xc9,0x5e,0xa8,0x50}};
    static std::wstring string(const BYTE*p,size_t size) {
        require(!(size&1));std::wstring s(size/2,L'\0');if(size)memcpy(s.data(),p,size);while(!s.empty()&&!s.back())s.pop_back();return s;
    }
    Input& input_;
    DWORD packet_size_{}, preroll_{}, object_number_{}, object_size_{}, object_time_{}, fragment_size_{}, maximum_{65536};
    uint64_t start_{}, count_{}, packet_number_{}, object_packet_{};
    BYTE stream_{}; bool partial_{},scanning_{};
    Bytes fragment_;
    struct Sample { Bytes data; DWORD time; uint64_t packet; };
    std::deque<Sample> samples_;
    struct Mark { uint64_t packet; DWORD time; };
    std::vector<Mark> marks_;
    DWORD selected_time_{};

    DWORD time(DWORD raw) const { return raw>preroll_?raw-preroll_:0; }
    void add(const BYTE* p,size_t n,DWORD stamp,uint64_t packet) {
        require(n && n<=64*1024*1024);maximum_=std::max(maximum_,DWORD(n));
        samples_.push_back({scanning_?Bytes{}:Bytes(p,p+n),time(stamp),packet});
    }
    void packet() {
        require(WaitForSingleObject(input_.event,0)!=WAIT_OBJECT_0,HRESULT_FROM_WIN32(ERROR_CANCELLED));
        Bytes bytes(packet_size_); input_.exact(bytes.data(),packet_size_);
        const auto index=packet_number_++;
        Cursor c{bytes.data(),bytes.size()};
        if(bytes[0]&0x80) { BYTE ec=c.byte(); require(!(ec&0x70),E_NOTIMPL); c.take(ec&15); }
        BYTE flags=c.byte(),properties=c.byte(); require((properties>>6)==1);
        DWORD length=c.field((flags>>5)&3); c.field((flags>>1)&3); DWORD padding=c.field((flags>>3)&3);
        DWORD stamp=c.field(3); c.field(2);
        if(!length)length=packet_size_;
        require(length<=packet_size_ && padding<=length && c.pos<=length-padding);
        c.size=length-padding;
        BYTE multiple=(flags&1)?c.byte():1; unsigned total=multiple&63;
        require(total>0);
        for(unsigned i=0;i<total;++i) {
            BYTE stream=c.byte()&127;
            DWORD object=c.field((properties>>4)&3), offset=c.field((properties>>2)&3), replicated=c.field(properties&3);
            require(replicated==0||replicated==1||replicated>=8);
            auto extra=c.take(replicated);
            size_t n=(flags&1)?c.field(multiple>>6):c.size-c.pos;
            auto p=c.take(n); if(stream!=stream_)continue;
            if(replicated==1) {
                Cursor sub{p,n}; DWORD t=offset;
                while(sub.pos<sub.size) { BYTE size=sub.byte(); require(size); auto data=sub.take(size); add(data,size,t,index); t+=*extra; }
                partial_=false; fragment_.clear();fragment_size_=0;
            } else {
                if(offset==0) {
                    if(partial_ && !object_size_ && fragment_size_)add(fragment_.data(),fragment_size_,object_time_,object_packet_);
                    else require(!partial_||!fragment_size_);
                    object_number_=object; object_size_=replicated>=8?le32(extra):0;
                    object_time_=replicated>=8?le32(extra+4):stamp;
                    require((replicated<8||object_size_) && object_size_<=64*1024*1024);
                    fragment_.clear();fragment_size_=0;object_packet_=index; partial_=true;
                }
                // A seek may start inside a fragmented media object. Ignore it
                // until the next complete object's first fragment is found.
                if(!partial_)continue;
                require(object==object_number_ && offset==fragment_size_);
                require(fragment_size_<=64*1024*1024 && n<=64*1024*1024-fragment_size_);
                if(object_size_)require(n<=object_size_-fragment_size_);
                if(replicated>=8)require(le32(extra)==object_size_);
                if(!scanning_)fragment_.insert(fragment_.end(),p,p+n);fragment_size_+=DWORD(n);
                if(fragment_size_==object_size_) { add(fragment_.data(),fragment_size_,object_time_,object_packet_); fragment_.clear();fragment_size_=0; partial_=false; }
            }
        }
        require(c.pos==c.size);
    }
    void reset(uint64_t packet) {input_.at(start_+packet*packet_size_);packet_number_=packet;samples_.clear();fragment_.clear();fragment_size_=0;partial_=false;}
    void index() {
        // A media object can be larger than one packet. Preflight packet
        // headers once, without retaining payload bytes, to advertise its real
        // maximum size and record safe object starts for later seeks.
        scanning_=true;uint64_t last{};
        while(packet_number_<count_) {
            packet();if(!samples_.empty()){
                const auto&s=samples_.front();
                if(marks_.size()<65536&&(marks_.empty()||s.packet>=last+64)){marks_.push_back({s.packet,s.time});last=s.packet;}
                samples_.clear();
            }
        }
        if(partial_&&!object_size_){maximum_=std::max(maximum_,fragment_size_);partial_=false;}
        require(!partial_,STG_E_READFAULT);scanning_=false;reset(0);
    }
public:
    struct Attribute { std::wstring name; WORD type; Bytes value; };
    std::vector<Attribute> attributes;
    Bytes format;
    DWORD duration{},bitrate{};
    explicit NativeAsf(Input& input):input_(input){}
    void open() {
        require(input_.seekable,STG_E_INVALIDFUNCTION); input_.at(input_.base);
        BYTE h[30]; input_.exact(h,sizeof(h)); require(guid(h,header_id));
        auto header_size=u64(h+16); DWORD objects=le32(h+24);
        require(header_size>=30 && header_size<=64*1024*1024 && objects<=65535);
        Bytes data(size_t(header_size-30)); input_.exact(data.data(),DWORD(data.size())); Cursor c{data.data(),data.size()};
        bool file=false;
        for(DWORD i=0;i<objects;++i) {
            auto object=c.take(24); uint64_t size=u64(object+16); require(size>=24 && size-24<=c.size-c.pos); auto p=c.take(size_t(size-24)); size-=24;
            if(guid(object,file_id)) {
                require(size>=80); uint64_t pre=u64(p+56), ticks=u64(p+40);
                require(pre<=MAXDWORD); preroll_=DWORD(pre); duration=dword(ticks/10000>pre?ticks/10000-pre:0);
                require(!(le32(p+64)&1),E_NOTIMPL); // finalised, bounded ASF file
                packet_size_=le32(p+72); require(packet_size_>=16 && packet_size_<=16*1024*1024 && le32(p+68)==packet_size_);
                bitrate=le32(p+76); file=true;
            } else if(guid(object,stream_id) && !stream_) {
                require(size>=54); if(!guid(p,audio_id))continue;
                DWORD types=le32(p+40), errors=le32(p+44); WORD flags=le16(p+48);
                require(!(flags&0x8000),E_NOTIMPL); require(types>=18 && types<=65536 && types<=size-54 && errors<=size-54-types);
                auto f=reinterpret_cast<const WAVEFORMATEX*>(p+54); require(f->nChannels && f->nSamplesPerSec && f->nBlockAlign);
                require(size_t(18)+f->cbSize<=types);
                // The system reader handles WMA's audio spread correction. This
                // path admits only codecs that need no interleaving transform.
                if(!guid(p+16,no_ec_id)) { require(guid(p+16,spread_id) && errors>=1 && p[54+types]<=1,E_NOTIMPL); }
                require(f->wFormatTag==WAVE_FORMAT_MPEGLAYER3,E_NOTIMPL);
                stream_=BYTE(flags&127); require(stream_); format.assign(p+54,p+54+18+f->cbSize);
            } else if(guid(object,content_id)) {
                require(size>=10);Cursor values{p,size_t(size),10};const wchar_t* names[]={L"Title",L"Author",L"Copyright",L"Description",L"Rating"};
                for(unsigned j=0;j<5;++j){auto n=le16(p+2*j);auto value=values.take(n);if(n)attributes.push_back({names[j],0,Bytes(value,value+n)});}
            } else if(guid(object,extended_id)) {
                Cursor values{p,size_t(size)};auto count=values.field(2);
                for(DWORD j=0;j<count;++j){auto n=values.field(2);auto name=string(values.take(n),n);WORD type=WORD(values.field(2));n=values.field(2);auto value=values.take(n);attributes.push_back({std::move(name),type,Bytes(value,value+n)});}
            }
        }
        require(c.pos==c.size && file && stream_,E_NOTIMPL);
        // Optional top-level objects may precede the mandatory Data Object.
        for(unsigned i=0;i<1024;++i) {
            BYTE object[24]; input_.exact(object,24); auto size=u64(object+16); require(size>=24);
            if(guid(object,data_id)) {
                require(size>=50); BYTE body[26]; input_.exact(body,26); count_=u64(body+16); start_=tell(input_.stream.p);
                require(count_ && count_<=(size-50)/packet_size_);
                require(start_<=input_.size && count_<=(input_.size-start_)/packet_size_,STG_E_READFAULT);index();return;
            }
            auto at=tell(input_.stream.p); require(at<=input_.size && size-24<=input_.size-at); input_.at(at+size-24);
        }
        require(false);
    }
    DWORD buffer_size() const { return maximum_; }
    bool read(Bytes& out) {
        while(samples_.empty() && packet_number_<count_)packet();
        if(samples_.empty() && partial_ && !object_size_) {add(fragment_.data(),fragment_size_,object_time_,object_packet_);fragment_.clear();fragment_size_=0;partial_=false;}
        if(samples_.empty()) { require(!partial_,STG_E_READFAULT); return false; }
        selected_time_=samples_.front().time; out=std::move(samples_.front().data); samples_.pop_front(); return true;
    }
    void seek_to(DWORD& target,Bytes& out) {
        target=std::min(target,duration); uint64_t at=0;
        if(target==duration){reset(count_);out.clear();return;}
        for(const auto&m:marks_) { if(m.time>target)break; at=m.packet; }
        reset(at);uint64_t chosen=at;
        {
            struct ScanScope {
                NativeAsf& self;
                explicit ScanScope(NativeAsf& value):self(value){self.scanning_=true;}
                ~ScanScope(){
                    self.scanning_=false;self.samples_.clear();self.fragment_.clear();
                    self.fragment_size_=0;self.partial_=false;
                    // Cancellation can interrupt a short read in the middle
                    // of a packet. Discard metadata-only fragments and leave
                    // the input at a packet boundary before Start retries.
                    protect([&]{self.input_.at(self.start_+self.packet_number_*self.packet_size_);return S_OK;});
                }
            } scan(*this);
            while(packet_number_<count_) {
                packet();if(samples_.empty())continue;
                const auto first=samples_.front();samples_.clear();
                if(first.time>target)break;chosen=first.packet;
            }
        }
        reset(chosen);
        if(read(out))target=chosen?selected_time_:0;else target=duration;
    }
};
}
