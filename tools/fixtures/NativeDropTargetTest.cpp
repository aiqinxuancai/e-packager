// 使用生产运行时代码验证 OLE 数据接收与窗口生命周期。
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <iostream>
#include <stdexcept>
using Value=std::wstring;
struct Unit { HWND hwnd; std::unordered_map<std::string,Value> properties; };
static std::unordered_map<unsigned,Unit> units;
static Unit* FindUnit(unsigned id){auto i=units.find(id);return i==units.end()?nullptr:&i->second;}
static std::wstring Wide(const char* s){int n=MultiByteToWideChar(CP_UTF8,0,s,-1,nullptr,0);std::wstring v(n,0);MultiByteToWideChar(CP_UTF8,0,s,-1,v.data(),n);v.pop_back();return v;}
static bool ToBool(const Value& v){return v==L"true";}
static Value Text(const std::string& s){return Wide(s.c_str());}
static Value TextFromWide(const std::wstring& s){return s;}
static std::vector<std::pair<int,Value>> events;
static std::function<void()> callback;
static void Dispatch(unsigned,int,int kind,std::vector<Value> values){events.emplace_back(kind,values.at(0));if(callback)callback();}
// INSERT_RUNTIME
static void Check(bool value,const char* label){if(!value)throw std::runtime_error(label);}
struct Data final : IDataObject {
    std::unordered_map<CLIPFORMAT,std::vector<char>> formats;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**) override{return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef() override{return 1;}
    ULONG STDMETHODCALLTYPE Release() override{return 1;}
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* f,STGMEDIUM* s) override {
        if(QueryGetData(f)!=S_OK)return DV_E_FORMATETC;
        const auto& bytes=formats.at(f->cfFormat);s->tymed=TYMED_HGLOBAL;s->pUnkForRelease=nullptr;
        s->hGlobal=GlobalAlloc(GHND,bytes.size());auto* out=GlobalLock(s->hGlobal);memcpy(out,bytes.data(),bytes.size());GlobalUnlock(s->hGlobal);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* f) override{return (f->tymed&TYMED_HGLOBAL)&&formats.contains(f->cfFormat)?S_OK:DV_E_FORMATETC;}
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*,STGMEDIUM*) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*,FORMATETC*) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*,STGMEDIUM*,BOOL) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD,IEnumFORMATETC**) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*,DWORD,IAdviseSink*,DWORD*) override{return OLE_E_ADVISENOTSUPPORTED;}
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override{return OLE_E_ADVISENOTSUPPORTED;}
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override{return OLE_E_ADVISENOTSUPPORTED;}
    void WideText(int kind,const std::wstring& value){auto p=reinterpret_cast<const char*>(value.c_str());formats[NativeDropTarget::Format(kind)]={p,p+(value.size()+1)*2};}
    void Files(){
        const wchar_t paths[]=L"C:\\图片\\甲.bmp\0D:\\乙.png\0";
        auto& b=formats[CF_HDROP];b.resize(sizeof(DROPFILES)+sizeof(paths));
        DROPFILES d{};d.pFiles=sizeof(d);d.fWide=TRUE;memcpy(b.data(),&d,sizeof(d));memcpy(b.data()+sizeof(d),paths,sizeof(paths));
    }
};
static HWND Window(){return CreateWindowW(L"STATIC",L"",WS_OVERLAPPED,0,0,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);}
int main(){try{
    HWND host=Window(),target=Window(),other=Window(),host2=Window();units[1].hwnd=host;units[2].hwnd=host2;
    Check(!RegisterNativeDropTarget(1,nullptr),"invalid window");
    Check(RegisterNativeDropTarget(1,target),"register");Check(RegisterNativeDropTarget(1,target),"idempotent");
    Check(!RegisterNativeDropTarget(2,target),"owner isolation");
    UnregisterNativeDropTarget(2,target);Check(nativeDropTargets.contains(target),"wrong-owner unregister");
    Data data;data.WideText(0,L"中文文本");data.WideText(2,L"https://example.invalid/");data.Files();
    auto& html=data.formats[NativeDropTarget::Format(1)];const std::string h="<b>hello</b>";html.assign(h.begin(),h.end());html.push_back(0);
    auto* object=nativeDropTargets.at(target);DWORD effect=DROPEFFECT_COPY|DROPEFFECT_MOVE;
    Check(object->DragEnter(&data,0,{},&effect)==S_OK&&effect==DROPEFFECT_COPY,"drag enter");
    object->Drop(&data,0,{},&effect);Check(events.size()==5&&events[0].second==L"中文文本"&&events[3].second==L"C:\\图片\\甲.bmp"&&events[4].second==L"D:\\乙.png","multiple formats / Unicode files");
    units[1].properties["接收文本"]=L"false";units[1].properties["接收超文本"]=L"false";units[1].properties["接收URL"]=L"false";
    events.clear();effect=DROPEFFECT_COPY;object->Drop(&data,0,{},&effect);Check(events.size()==2&&events[0].first==3,"format filtering");
    events.clear();effect=DROPEFFECT_MOVE;object->Drop(&data,0,{},&effect);Check(events.empty()&&effect==0,"no source deletion");
    Check(RegisterNativeDropTarget(1,other),"replace target");Check(!nativeDropTargets.contains(target)&&nativeDropTargets.size()==1,"single target");
    object=nativeDropTargets.at(other);events.clear();callback=[&]{UnregisterNativeDropTarget(1,other);};effect=DROPEFFECT_COPY;
    object->Drop(&data,0,{},&effect);callback={};Check(events.size()==1&&nativeDropTargets.empty(),"reentrant revoke stops multi-file delivery");
    Check(RegisterNativeDropTarget(1,target),"register again");DestroyWindow(target);Check(nativeDropTargets.empty(),"target destruction");
    Check(RegisterNativeDropTarget(1,other),"register provider destruction");
    object=nativeDropTargets.at(other);events.clear();callback=[&]{DestroyWindow(host);};effect=DROPEFFECT_COPY;object->Drop(&data,0,{},&effect);callback={};
    Check(events.size()==1&&nativeDropTargets.empty(),"provider destruction during callback");
    DestroyWindow(other);DestroyWindow(host2);std::cout<<"PASS: OLE formats, filtering, Unicode/multiple files, registration, reentrant revoke and destruction\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
