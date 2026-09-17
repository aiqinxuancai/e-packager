#include "SemanticOptimization.h"
#include "NativeWindowControl.h"
#include <algorithm>
namespace ecompiler {
namespace {
class Analyzer {
    const Program& p;
    const BindOptimizationCall& bind;
    OptimizationAnalysis result;
    std::vector<std::size_t> pending;
    void retain(std::size_t id, const std::string& reason) {
        result.reasons[id].insert(reason);
        if(result.reachable.insert(id).second)pending.push_back(id);
    }
    void native(std::size_t method, std::size_t line, const std::string& reason,
        const std::string& target = {}, std::uint32_t code = 0) {
        result.opaqueNativeAccess = true;
        result.nativeBoundaries.push_back({method,line,code,reason,target});
    }
    void type(std::uint32_t code) {
        code &= ~kTypeArrayFlag;
        if(!result.instantiated.insert(code).second)return;
        const auto* t=p.FindType(code);if(!t)return;
        if(!t->isEnum && t->libraryIndex<p.libraries.size())native(static_cast<std::size_t>(-1),0,"native_type_lifecycle",t->name,code);
        if(t->baseType.valid)type(t->baseType.code);
        for(const auto& f:t->elements)type(f.type.code);
        for(auto id:t->memberMethodIds) {
            const auto& m=p.methods[id];
            if(m.name=="_初始化"||m.name=="_销毁")retain(id,"lifecycle:"+std::to_string(code));
        }
    }
    void expression(const Method& owner,const e2txt::SourceExpressionNode* n, std::size_t line) {
        if(!n)return;
        using K=e2txt::SourceExpressionKind;
        if(n->kind==K::Call||n->kind==K::AddressOf) {
            auto b=bind(owner,*n);result.calls[n]=b;
            if(b.target) {
                result.edges[owner.id].insert(b.target->id);
                retain(b.target->id,"call:"+std::to_string(owner.id));
                if(n->kind==K::AddressOf)result.callbacks.insert(b.target->id);
            }
            if(b.nativeBoundary) {
                const auto target = !n->children.empty() ? n->children.front()->text : n->text;
                native(owner.id,line,n->kind==K::AddressOf?"callback_address":"external_call",target);
            }
            // 调用目标不是值表达式；参数及成员接收者仍需遍历。
            if(n->kind==K::Call&&!n->children.empty()) {
                const auto& c=*n->children[0];
                if(c.kind==K::Member)for(const auto& x:c.children)expression(owner,x.get(),line);
                for(std::size_t i=1;i<n->children.size();++i)expression(owner,n->children[i].get(),line);
            }
            return;
        }
        for(const auto& c:n->children)expression(owner,c.get(),line);
    }
    void statements(const Method& m,const std::vector<Statement>& ss) {
        for(const auto& s:ss) {
            if(s.kind==StatementKind::MachineCode)native(m.id,s.sourceLine,"machine_code");
            expression(m,s.expression.get(),s.sourceLine);expression(m,s.target.get(),s.sourceLine);
            for(const auto& a:s.arguments)expression(m,a.get(),s.sourceLine);
            statements(m,s.body);statements(m,s.elseBody);
            for(const auto& b:s.branches){expression(m,b.condition.get(),s.sourceLine);statements(m,b.body);}
        }
    }
public:
    Analyzer(const Program& program,const BindOptimizationCall& binder):p(program),bind(binder){}
    OptimizationAnalysis run() {
        if(auto it=p.methodByName.find("_启动子程序");it!=p.methodByName.end())retain(it->second,"startup");
        for(const auto& n:p.moduleStartupNames)retain(p.methodByName.at(n),"module_startup");
        for(const auto& f:p.windows) {
            for(const auto& e:f.events)retain(e.methodId,"window_event");
            for(const auto& c:f.controls)if(HasNativeWin32Class(c.typeName))for(const auto& e:c.events)retain(e.methodId,"control_event");
        }
        for(const auto& v:p.globals)type(v.type.code);
        for(const auto& a:p.assemblies)for(const auto& v:a.variables)type(v.type.code);
        for(const auto& m:p.methods)if(p.buildDll&&m.isPublic){retain(m.id,"dll_export");native(m.id,m.sourceLine,"dll_export",m.name);}
        std::size_t cursor=0;
        for(;;) {
            while(cursor<pending.size()) {
                const auto& m=p.methods[pending[cursor++]];
                if(m.ownerType.valid)type(m.ownerType.code);
                for(const auto& v:m.locals)type(v.type.code);
                for(const auto& v:m.parameters)type(v.type.code);
                if(m.returnType.valid)type(m.returnType.code);
                statements(m,m.body);
            }
            auto before=pending.size();
            if(result.opaqueNativeAccess) {
                // 无法证明原生内存观察范围时沿用原生方法表闭包，不按库名猜测。
                for(const auto& m:p.methods)if(m.ownerType.valid&&m.name=="_初始化")retain(m.id,"opaque_native_lifecycle");
                auto reachable=result.reachable;
                for(auto id:reachable)if(const auto* t=p.FindType(p.methods[id].ownerType.code))
                    for(auto member:t->memberMethodIds)retain(member,"opaque_native_class_table");
            }
            auto reachable=result.reachable;
            for(auto id:reachable) {
                const auto& m=p.methods[id];
                if(!m.ownerType.valid||m.name=="_初始化"||m.name=="_销毁")continue;
                for(const auto& t:p.types) {
                    if(!result.opaqueNativeAccess&&!result.instantiated.contains(t.type.code))continue;
                    const auto* a=p.FindType(t.baseType.code);
                    while(a&&a->type.code!=m.ownerType.code)a=p.FindType(a->baseType.code);
                    if(!a)continue;
                    for(auto member:t.memberMethodIds)if(p.methods[member].name==m.name){retain(member,"virtual_dispatch:"+std::to_string(id));break;}
                }
            }
            if(before==pending.size())break;
        }
        return std::move(result);
    }
};
}
OptimizationAnalysis AnalyzeSemanticReachability(const Program& p,const BindOptimizationCall& bind){return Analyzer(p,bind).run();}
}
