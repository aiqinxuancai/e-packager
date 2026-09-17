#include "TypedScalarEmitter.h"
#include <sstream>

namespace ecompiler {
namespace {
bool Scalar(TypeRef t) {
    if(t.isArray)return false;
    switch(t.code) {
    case kTypeByte: case kTypeShort: case kTypeInt: case kTypeInt64:
    case kTypeFloat: case kTypeDouble: case kTypeDateTime: case kTypeBool: return true;
    default:return false;
    }
}
bool Floating(std::uint32_t t){return t==kTypeFloat||t==kTypeDouble||t==kTypeDateTime;}
std::string CType(std::uint32_t t){return t==kTypeNull?"void":t==kTypeBool?"bool":Floating(t)?"double":"long long";}
std::string Cast(std::uint32_t t,const std::string& x){return "static_cast<"+CType(t)+">("+x+")";}
std::string Signature(const Method& m) {
    std::string s="static "+CType(m.returnType.code)+" scalar_"+std::to_string(m.id)+"(";
    for(std::size_t i=0;i<m.parameters.size();++i){if(i)s+=',';s+=CType(m.parameters[i].type.code)+" p"+std::to_string(i);}
    return s+")";
}
struct Expression {std::string code;std::uint32_t type=kTypeNull;};
class Generator {
    const Method& m;
    const OptimizationAnalysis& analysis;
    const std::set<std::size_t>& candidates;
    std::map<std::string,Expression> variables;
    std::ostringstream out;
    std::string reason;
    Expression reject(const std::string& r){if(reason.empty())reason=r;return {"0",kTypeInt};}
    Expression expr(const e2txt::SourceExpressionNode* n) {
        using K=e2txt::SourceExpressionKind;
        if(!n)return reject("missing_expression");
        switch(n->kind) {
        case K::NumberLiteral:
            return {n->text+(n->text.find('.')==std::string::npos?"LL":""),n->text.find('.')==std::string::npos?kTypeInt:kTypeDouble};
        case K::LogicalLiteral:return {n->text=="真"?"true":"false",kTypeBool};
        case K::Name: {auto it=variables.find(n->text);if(it!=variables.end())return it->second;return reject("global_constant_or_nonlocal_access");}
        case K::Group:if(n->children.size()==1)return expr(n->children[0].get());return reject("invalid_group");
        case K::Unary: {
            if(n->children.size()!=1)return reject("invalid_unary");
            auto a=expr(n->children[0].get());
            if(a.type==kTypeNull)return reject("void_operand");
            if(n->text=="-"||n->text=="－")return {"(-"+Cast(kTypeDouble,a.code)+")",kTypeDouble};
            if(n->text=="!")return {"(!"+Cast(kTypeBool,a.code)+")",kTypeBool};
            return a;
        }
        case K::Binary: {
            if(n->children.size()!=2)return reject("invalid_binary");
            auto a=expr(n->children[0].get()), b=expr(n->children[1].get());
            if(a.type==kTypeNull||b.type==kTypeNull)return reject("void_operand");
            const auto x=Cast(kTypeDouble,a.code),y=Cast(kTypeDouble,b.code);
            const auto& op=n->text;
            if(op=="+"||op=="＋")return {"("+x+"+"+y+")",kTypeDouble};
            if(op=="-"||op=="－")return {"("+x+"-"+y+")",kTypeDouble};
            if(op=="*"||op=="×")return {"("+x+"*"+y+")",kTypeDouble};
            if(op=="/"||op=="÷")return {"scalar_div("+x+","+y+")",kTypeDouble};
            // 整除与取余的 MIN/-1 行为尚未定义，暂不降低为 C++ 运算。
            if(op=="\\"||op=="%"||op=="％")return reject("integer_division_overflow_semantics");
            if(op=="="||op=="＝"||op=="=="||op=="?=")return {"("+x+"=="+y+")",kTypeBool};
            if(op=="!="||op=="≠"||op=="<>")return {"("+x+"!="+y+")",kTypeBool};
            if(op=="<"||op=="＜")return {"("+x+"<"+y+")",kTypeBool};
            if(op=="<="||op=="≤")return {"("+x+"<="+y+")",kTypeBool};
            // 通用路径 Gt/Ge 使用否定比较，保留 NaN 时的既有行为。
            if(op==">"||op=="＞")return {"(!("+x+"<="+y+"))",kTypeBool};
            if(op==">="||op=="≥")return {"(!("+x+"<"+y+"))",kTypeBool};
            if(op=="&"||op=="且")return {"("+Cast(kTypeBool,a.code)+" & "+Cast(kTypeBool,b.code)+")",kTypeBool};
            if(op=="|"||op=="或")return {"("+Cast(kTypeBool,a.code)+" | "+Cast(kTypeBool,b.code)+")",kTypeBool};
            return reject("unsupported_operator");
        }
        case K::Call: {
            const auto it=analysis.calls.find(n);
            if(it==analysis.calls.end()||!it->second.target)return reject("external_call");
            const auto& target=*it->second.target;
            if(!candidates.contains(target.id))return reject("generic_callee:"+std::to_string(target.id));
            if(n->children.size()!=target.parameters.size()+1)return reject("omitted_argument");
            std::string call="scalar_"+std::to_string(target.id)+"(";
            for(std::size_t i=1;i<n->children.size();++i){if(i>1)call+=',';auto a=expr(n->children[i].get());if(a.type==kTypeNull)return reject("void_argument");call+=Cast(target.parameters[i-1].type.code,a.code);}
            return {call+")",target.returnType.code};
        }
        default:return reject("complex_or_address_expression");
        }
    }
    std::string condition(const e2txt::SourceExpressionNode* node) {
        auto value=expr(node);
        if(value.type==kTypeNull)reject("void_condition");
        return Cast(kTypeBool,value.code);
    }
    Expression counter(const e2txt::SourceExpressionNode* node) {
        if(!node||node->kind!=e2txt::SourceExpressionKind::Name)return reject("nonlocal_loop_counter");
        return expr(node);
    }
    void line(int depth,const std::string& text){out<<std::string(depth*4,' ')<<text<<'\n';}
    void statements(const std::vector<Statement>& ss,int depth) {
        for(const auto& s:ss) {
            if(s.sourceLine)out<<"#line "<<s.sourceLine<<'\n';
            switch(s.kind) {
            case StatementKind::Expression:line(depth,expr(s.expression.get()).code+";");break;
            case StatementKind::Assignment: {
                if(!s.target||s.target->kind!=e2txt::SourceExpressionKind::Name){reject("nonlocal_assignment");break;}
                auto t=expr(s.target.get()),v=expr(s.expression.get());
                if(v.type==kTypeNull){reject("void_assignment");break;}
                line(depth,t.code+"="+Cast(t.type,v.code)+";");break;
            }
            case StatementKind::Return:
                if(!s.expression){if(m.returnType.code!=kTypeNull)reject("empty_scalar_return");line(depth,"return;");}
                else {auto v=expr(s.expression.get());if(v.type!=m.returnType.code)reject("dynamic_return_type");line(depth,"return "+v.code+";");}break;
            case StatementKind::IfTrue: case StatementKind::IfElse:
                line(depth,"if("+condition(s.expression.get())+") {");statements(s.body,depth+1);line(depth,"}");
                if(!s.elseBody.empty()){line(depth,"else {");statements(s.elseBody,depth+1);line(depth,"}");}break;
            case StatementKind::While:
                line(depth,"while("+condition(s.expression.get())+") {");statements(s.body,depth+1);line(depth,"}");break;
            case StatementKind::DoWhile:
                line(depth,"do {");statements(s.body,depth+1);line(depth,"} while("+condition(s.expression.get())+");");break;
            case StatementKind::Switch:
                for(std::size_t i=0;i<s.branches.size();++i) {
                    line(depth,std::string(i?"else if(":"if(")+condition(s.branches[i].condition.get())+") {");
                    statements(s.branches[i].body,depth+1);line(depth,"}");
                }
                if(!s.elseBody.empty()) {line(depth,s.branches.empty()?"{":"else {");statements(s.elseBody,depth+1);line(depth,"}");}
                break;
            case StatementKind::CountLoop: {
                if(s.arguments.empty()){reject("missing_loop_arguments");break;}
                auto limit=expr(s.arguments[0].get());
                if(limit.type==kTypeNull){reject("void_loop_bound");break;}
                const bool hasCounter=s.arguments.size()>1&&s.arguments[1]->kind!=e2txt::SourceExpressionKind::Missing;
                Expression index;
                if(hasCounter)index=counter(s.arguments[1].get());
                line(depth,"for(int limit=static_cast<int>("+Cast(kTypeInt64,limit.code)+"),i=1;i<=limit;++i) {");
                if(hasCounter)line(depth+1,index.code+"="+Cast(index.type,"i")+";");
                statements(s.body,depth+1);line(depth,"}");break;
            }
            case StatementKind::ForLoop: {
                if(s.arguments.size()<3){reject("missing_loop_arguments");break;}
                auto begin=expr(s.arguments[0].get()),end=expr(s.arguments[1].get()),step=expr(s.arguments[2].get());
                if(begin.type==kTypeNull||end.type==kTypeNull||step.type==kTypeNull){reject("void_loop_bound");break;}
                const bool hasCounter=s.arguments.size()>3&&s.arguments[3]->kind!=e2txt::SourceExpressionKind::Missing;
                Expression index;
                if(hasCounter)index=counter(s.arguments[3].get());
                line(depth,"{ double begin="+Cast(kTypeDouble,begin.code)+",end="+Cast(kTypeDouble,end.code)+",step="+Cast(kTypeDouble,step.code)+";");
                line(depth+1,"if(step!=0) for(double i=begin;step>0?i<=end:i>=end;i+=step) {");
                if(hasCounter)line(depth+2,index.code+"="+Cast(index.type,"i")+";");
                statements(s.body,depth+2);line(depth+1,"}");line(depth,"}");break;
            }
            case StatementKind::Break:line(depth,"break;");break;
            case StatementKind::Continue:line(depth,"continue;");break;
            default:reject("unsupported_statement:"+std::to_string(static_cast<int>(s.kind)));break;
            }
        }
    }
public:
    Generator(const Program& program,const Method& method,const OptimizationAnalysis& a,const std::set<std::size_t>& c):m(method),analysis(a),candidates(c){ (void)program; }
    std::pair<std::string,std::string> run() {
        if(m.ownerType.valid)return {"","class_method"};
        if(m.returnType.code!=kTypeNull&&!Scalar(m.returnType))return {"","complex_return"};
        if(m.returnType.code!=kTypeNull&&(m.body.empty()||m.body.back().kind!=StatementKind::Return))return {"","possible_fallthrough"};
        for(std::size_t i=0;i<m.parameters.size();++i) {
            const auto& v=m.parameters[i];
            if(!Scalar(v.type)||v.byReference||v.nullable)return {"","complex_reference_or_optional_parameter"};
            variables.emplace(v.name,Expression{"p"+std::to_string(i),v.type.code});
        }
        out<<Signature(m)<<" {\n";
        for(std::size_t i=0;i<m.locals.size();++i) {
            const auto& v=m.locals[i];
            if(!Scalar(v.type)||v.isStatic)return {"","complex_or_static_local"};
            const auto name="v"+std::to_string(i);
            variables.emplace(v.name,Expression{name,v.type.code});line(1,CType(v.type.code)+" "+name+"=0;");
        }
        statements(m.body,1);out<<"}\n";
        if(!reason.empty())return {"",reason};
        out<<"static Value method_"<<m.id<<"(std::vector<Arg> a,Value*,bool) {\n";
        std::string call="scalar_"+std::to_string(m.id)+"(";
        for(std::size_t i=0;i<m.parameters.size();++i) {
            if(i)call+=',';auto t=m.parameters[i].type.code;
            call+="a.size()>"+std::to_string(i)+"?"+(t==kTypeBool?"ToBool":Floating(t)?"ToNumber":"ToInteger")+std::string("(a[")+std::to_string(i)+"].Get()):0";
        }
        call+=")";
        if(m.returnType.code==kTypeNull)out<<"    "<<call<<"; return Empty();\n";
        else if(m.returnType.code==kTypeBool)out<<"    return Boolean("<<call<<");\n";
        else if(Floating(m.returnType.code))out<<"    Value r=Number("<<call<<");r.type=r.declared="<<m.returnType.code<<"u;return r;\n";
        else out<<"    return Integer("<<call<<","<<m.returnType.code<<"u);\n";
        out<<"}\n";return {out.str(),""};
    }
};
}
TypedScalarOutput GenerateTypedScalarMethods(const Program& p,const OptimizationAnalysis& analysis) {
    TypedScalarOutput result;
    auto candidates=analysis.reachable;
    // 被降级方法的调用者随之降级；递归的纯标量调用组可整体保留。
    for(;;) {
        std::set<std::size_t> removed;
        for(auto id:candidates) {
            auto [code,reason]=Generator(p,p.methods[id],analysis,candidates).run();
            if(!reason.empty()){result.rejected[id]=reason;removed.insert(id);}
        }
        if(removed.empty())break;
        for(auto id:removed)candidates.erase(id);
    }
    result.declarations="static double scalar_div(double a,double b){return b==0?0:a/b;}\n";
    for(auto id:candidates){result.declarations+=Signature(p.methods[id])+";\n";result.definitions[id]=Generator(p,p.methods[id],analysis,candidates).run().first;}
    return result;
}
}
