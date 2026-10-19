#include "common.h"
namespace ttp::asf {
HMODULE module{};
HMODULE system_library(const wchar_t*name){wchar_t path[MAX_PATH]{};UINT n=GetSystemDirectoryW(path,MAX_PATH);require(n&&n<MAX_PATH);std::wstring full=std::wstring(path)+L"\\"+name;auto h=LoadLibraryW(full.c_str());require(h,HRESULT_FROM_WIN32(GetLastError()));return h;}
class ReaderFactory final:public ReaderCreator{
 LONG refs_{1};public:
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void**out)override{if(!out)return E_POINTER;*out=nullptr;if(!same(id,IID_IUnknown)&&!same(id,cat_reader))return E_NOINTERFACE;*out=this;AddRef();return S_OK;}
 ULONG STDMETHODCALLTYPE AddRef()override{return InterlockedIncrement(&refs_);}ULONG STDMETHODCALLTYPE Release()override{auto n=InterlockedDecrement(&refs_);if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE Create(void**out)override{return make_reader(out);}
 HRESULT STDMETHODCALLTYPE Name(wchar_t**out)override{return text(L"ASF Reader",out);}
 HRESULT STDMETHODCALLTYPE Extensions(wchar_t**out)override{return text(L"Windows Media 音频文件(*.wma;*.wmv;*.asf)",out);}
};
class DecoderFactory final:public DecoderCreator{
 LONG refs_{1};public:
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void**out)override{if(!out)return E_POINTER;*out=nullptr;if(!same(id,IID_IUnknown)&&!same(id,cat_decoder))return E_NOINTERFACE;*out=this;AddRef();return S_OK;}
 ULONG STDMETHODCALLTYPE AddRef()override{return InterlockedIncrement(&refs_);}ULONG STDMETHODCALLTYPE Release()override{auto n=InterlockedDecrement(&refs_);if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE Create(void**out)override{return make_decoder(out);}
 HRESULT STDMETHODCALLTYPE Name(wchar_t**out)override{return text(L"DMO Decoder",out);}
 // Original 601556F7 is a catch-all candidate: S_FALSE is still success.
 HRESULT STDMETHODCALLTYPE Supports(const GUID*id)override{return id?S_FALSE:E_POINTER;}
};
class Plugin final:public AddIn{
 LONG refs_{1};public:
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void**out)override{if(!out)return E_POINTER;*out=nullptr;if(!same(id,IID_IUnknown)&&!same(id,iid_addin))return E_NOINTERFACE;*out=this;AddRef();return S_OK;}
 ULONG STDMETHODCALLTYPE AddRef()override{return InterlockedIncrement(&refs_);}ULONG STDMETHODCALLTYPE Release()override{auto n=InterlockedDecrement(&refs_);if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE Enum(DWORD i,GUID*cat,void**out)override{if(!out)return E_POINTER;*out=nullptr;if(!cat)return E_POINTER;return protect([&]{if(i==0){*cat=cat_reader;*out=static_cast<ReaderCreator*>(new ReaderFactory);}else if(i==1){*cat=cat_decoder;*out=static_cast<DecoderCreator*>(new DecoderFactory);}else return E_INVALIDARG;return S_OK;});}
};
}
extern "C" HRESULT WINAPI ttpGetSoundAddIn(void**out){if(!out)return E_POINTER;*out=nullptr;return ttp::asf::protect([&]{*out=static_cast<ttp::asf::AddIn*>(new ttp::asf::Plugin);return S_OK;});}
BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH){ttp::asf::module=h;DisableThreadLibraryCalls(h);}return TRUE;}
