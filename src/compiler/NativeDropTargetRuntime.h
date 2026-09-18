#pragma once

namespace ecompiler {

// 独立窗口运行时的 OLE 拖放管理；非可视提供者与目标 HWND 分开管理生命周期。
inline constexpr const char* kNativeDropTargetRuntime = R"CPP(
static Value TextFromWide(const std::wstring& wide);
struct NativeDropTarget;
static thread_local std::unordered_map<HWND,NativeDropTarget*> nativeDropTargets;
static void RevokeNativeDropTarget(HWND target);
static LRESULT CALLBACK NativeDropSubclass(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
struct NativeDropOleScope {
    HRESULT status=OleInitialize(nullptr);
    ~NativeDropOleScope(){if(SUCCEEDED(status))OleUninitialize();}
};
static bool NativeDropEnabled(unsigned owner,int kind) {
    auto* unit=FindUnit(owner);
    if(!unit||!IsWindow(unit->hwnd)||!IsWindowEnabled(unit->hwnd))return false;
    static const wchar_t* names[]={L"\u63a5\u6536\u6587\u672c",L"\u63a5\u6536\u8d85\u6587\u672c",L"\u63a5\u6536URL",L"\u63a5\u6536\u6587\u4ef6"};
    for(const auto& item:unit->properties)if(Wide(item.first.c_str())==names[kind])return ToBool(item.second);
    return true;
}
static void NativeDropFiles(unsigned owner,HWND target,NativeDropTarget* registration,HDROP files) {
    const UINT count=DragQueryFileW(files,0xFFFFFFFFu,nullptr,0);
    for(UINT i=0;i<count&&NativeDropEnabled(owner,3)&&nativeDropTargets.contains(target)&&nativeDropTargets.at(target)==registration;++i) {
        const UINT size=DragQueryFileW(files,i,nullptr,0);
        std::wstring path(size+1,L'\0');DragQueryFileW(files,i,path.data(),size+1);path.resize(size);
        Dispatch(owner,0,3,{TextFromWide(path)});
    }
}
struct NativeDropTarget final : IDropTarget {
    LONG references=1;
    unsigned owner;
    HWND provider,target;
    bool hadAcceptFiles;
    bool offered=false;
    NativeDropTarget(unsigned id,HWND host,HWND window):owner(id),provider(host),target(window),
        hadAcceptFiles((GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_ACCEPTFILES)!=0){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result) override {
        if(!result)return E_POINTER;*result=nullptr;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(IDropTarget)){*result=static_cast<IDropTarget*>(this);AddRef();return S_OK;}
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return InterlockedIncrement(&references);}
    ULONG STDMETHODCALLTYPE Release() override{const auto count=InterlockedDecrement(&references);if(!count)delete this;return count;}
    static CLIPFORMAT Format(int kind,bool unicode=true) {
        if(kind==0)return unicode?CF_UNICODETEXT:CF_TEXT;
        if(kind==1)return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"HTML Format"));
        if(kind==2)return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(unicode?L"UniformResourceLocatorW":L"UniformResourceLocator"));
        return CF_HDROP;
    }
    bool Offered(IDataObject* data) {
        if(!data)return false;
        for(int kind=0;kind<4;++kind)if(NativeDropEnabled(owner,kind))for(bool wide:{true,false}) {
            FORMATETC format{Format(kind,wide),nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
            if(data->QueryGetData(&format)==S_OK)return true;
        }
        return false;
    }
    static DWORD Effect(DWORD allowed,bool accepted) {
        // 只接收副本；没有执行源数据删除，不能向拖动源报告 MOVE 成功。
        return accepted?(allowed&DROPEFFECT_COPY):DROPEFFECT_NONE;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data,DWORD,POINTL,DWORD* effect) override {
        if(!effect)return E_POINTER;offered=Offered(data);*effect=Effect(*effect,offered);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD,POINTL,DWORD* effect) override {
        if(!effect)return E_POINTER;*effect=Effect(*effect,offered);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override{offered=false;return S_OK;}
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data,DWORD,POINTL,DWORD* effect) override {
        if(!data||!effect)return E_POINTER;
        const DWORD allowed=*effect;*effect=DROPEFFECT_NONE;
        if(!(allowed&DROPEFFECT_COPY))return S_OK;
        // 用户事件允许注销目标或销毁窗体；保留本次回调的对象引用。
        AddRef();struct Pin{NativeDropTarget* value;~Pin(){value->Release();}}pin{this};
        bool received=false;
        for(int kind=0;kind<4;++kind) {
            if(!IsWindow(provider)||!IsWindow(target)||!nativeDropTargets.contains(target)||nativeDropTargets.at(target)!=this)break;
            if(!NativeDropEnabled(owner,kind))continue;
            for(bool wide:{true,false}) {
                FORMATETC format{Format(kind,wide),nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
                STGMEDIUM medium{};
                if(FAILED(data->GetData(&format,&medium)))continue;
                struct Medium{STGMEDIUM* value;~Medium(){ReleaseStgMedium(value);}}release{&medium};
                if(medium.tymed!=TYMED_HGLOBAL||!medium.hGlobal)break;
                if(kind==3){NativeDropFiles(owner,target,this,reinterpret_cast<HDROP>(medium.hGlobal));received=true;break;}
                const SIZE_T size=GlobalSize(medium.hGlobal);
                const auto* bytes=static_cast<const char*>(GlobalLock(medium.hGlobal));
                if(!bytes)break;
                struct Lock{HGLOBAL value;~Lock(){GlobalUnlock(value);}}unlock{medium.hGlobal};
                Value text;
                if(wide&&kind!=1) {
                    const auto* chars=reinterpret_cast<const wchar_t*>(bytes);std::size_t length=0;
                    while(length<size/sizeof(wchar_t)&&chars[length])++length;
                    text=TextFromWide(std::wstring(chars,length));
                } else {
                    std::size_t length=0;while(length<size&&bytes[length])++length;
                    if(kind==1) {
                        const int count=MultiByteToWideChar(CP_UTF8,0,bytes,static_cast<int>(length),nullptr,0);
                        std::wstring html(count,L'\0');if(count)MultiByteToWideChar(CP_UTF8,0,bytes,static_cast<int>(length),html.data(),count);
                        text=TextFromWide(html);
                    } else text=Text(std::string(bytes,length));
                }
                Dispatch(owner,0,kind,{std::move(text)});received=true;break;
            }
        }
        offered=false;*effect=Effect(allowed,received);return S_OK;
    }
};
static void RevokeNativeDropTarget(HWND target) {
    const auto found=nativeDropTargets.find(target);if(found==nativeDropTargets.end())return;
    auto* object=found->second;nativeDropTargets.erase(found);
    RevokeDragDrop(target);
    if(IsWindow(target)){DragAcceptFiles(target,object->hadAcceptFiles);RemoveWindowSubclass(target,NativeDropSubclass,0xE1D4u);}
    object->Release();
}
static LRESULT CALLBACK NativeDropSubclass(HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR) {
    if(message==WM_DROPFILES) {
        const auto found=nativeDropTargets.find(window);
        if(found!=nativeDropTargets.end()) {
            auto* object=found->second;object->AddRef();
            struct Pin{NativeDropTarget* value;~Pin(){value->Release();}}pin{object};
            const auto owner=object->owner;
            struct Finish{HDROP value;~Finish(){DragFinish(value);}}finish{reinterpret_cast<HDROP>(w)};
            NativeDropFiles(owner,window,object,finish.value);return 0;
        }
    }
    if(message==WM_NCDESTROY) {
        // 同时处理目标销毁与隐藏提供者销毁，使用快照避免回调期间迭代失效。
        std::vector<HWND> targets;
        for(const auto& item:nativeDropTargets)if(item.first==window||item.second->provider==window)targets.push_back(item.first);
        for(HWND target:targets)RevokeNativeDropTarget(target);
        RemoveWindowSubclass(window,NativeDropSubclass,0xE1D3u);
        RemoveWindowSubclass(window,NativeDropSubclass,0xE1D4u);
    }
    return DefSubclassProc(window,message,w,l);
}
static bool RegisterNativeDropTarget(unsigned owner,HWND target) {
    auto* unit=FindUnit(owner);
    if(!unit||!IsWindow(unit->hwnd)||!IsWindow(target)||unit->hwnd==target||
       GetWindowThreadProcessId(target,nullptr)!=GetCurrentThreadId())return false;
    static thread_local NativeDropOleScope ole;
    if(FAILED(ole.status))return false;
    if(const auto existing=nativeDropTargets.find(target);existing!=nativeDropTargets.end())return existing->second->owner==owner;
    auto* object=new NativeDropTarget(owner,unit->hwnd,target);
    if(FAILED(RegisterDragDrop(target,object))){object->Release();return false;}
    if(!SetWindowSubclass(unit->hwnd,NativeDropSubclass,0xE1D3u,0)||!SetWindowSubclass(target,NativeDropSubclass,0xE1D4u,0)) {
        RevokeDragDrop(target);object->Release();return false;
    }
    std::vector<HWND> previous;
    for(const auto& item:nativeDropTargets)if(item.second->owner==owner)previous.push_back(item.first);
    for(HWND old:previous)RevokeNativeDropTarget(old);
    nativeDropTargets.emplace(target,object);DragAcceptFiles(target,TRUE);return true;
}
static void UnregisterNativeDropTarget(unsigned owner,HWND target) {
    const auto found=nativeDropTargets.find(target);
    if(found!=nativeDropTargets.end()&&found->second->owner==owner)RevokeNativeDropTarget(target);
}
)CPP";
} // namespace ecompiler
