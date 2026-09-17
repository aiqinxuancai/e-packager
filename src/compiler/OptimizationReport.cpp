#include "OptimizationReport.h"
#include "../PathHelper.h"
#include "../../thirdparty/json.hpp"
#include <Windows.h>
#include <fstream>
#include <cstring>
#include <stdexcept>

namespace ecompiler {
namespace {
std::string SourceUtf8(const std::string& s) {
    if(s.empty())return s;
    int count=MultiByteToWideChar(936,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0);
    if(!count)throw std::runtime_error("invalid_source_encoding");
    std::wstring wide(count,L'\0');MultiByteToWideChar(936,0,s.data(),static_cast<int>(s.size()),wide.data(),count);
    return WideToUtf8Text(wide);
}
}
bool WriteOptimizationReport(const std::filesystem::path& path,const std::filesystem::path& executable,
    const Program& p,const GeneratedSource& g,SemanticOptimization mode,double elapsed,std::string& error,bool optimizeForSize) {
    using nlohmann::json;
    try {
        json j={{"schema_version",1},{"codegen_optimization",optimizeForSize?"size":"speed"},{"mode",mode==SemanticOptimization::Baseline?"baseline":mode==SemanticOptimization::Reachable?"reachable":"typed"},
            {"architecture",p.targetArchitecture==TargetArchitecture::X86?"x86":"x64"},
            {"total_methods",p.methods.size()},{"reachable_methods",g.reachableMethodCount},{"typed_methods",g.typedMethods.size()},
            {"generic_methods",g.reachableMethodCount-g.typedMethods.size()},{"native_wrappers",g.nativeWrapperCount},
            {"opaque_native_access",g.optimization.opaqueNativeAccess},{"commands",g.reachableCommandCount},
            {"libraries",g.reachableLibraries.size()},{"generated_source_buffer_bytes",g.text.size()},
            {"analysis_enabled",mode!=SemanticOptimization::Baseline},
            {"compile_elapsed_ms",elapsed},{"executable_bytes",std::filesystem::file_size(executable)}};
        auto source=executable;source.replace_extension(L".generated.cpp");
        j["generated_source_bytes"]=std::filesystem::file_size(source);
        j["native_boundaries"]=json::array();
        std::map<std::string,std::size_t> boundaryCounts;
        for(const auto& site:g.optimization.nativeBoundaries) {
            json row={{"reason",site.reason},{"target",SourceUtf8(site.target)},
                {"line",site.sourceLine},{"type_code",site.typeCode}};
            if(site.methodId<p.methods.size()) {
                const auto& owner=p.methods[site.methodId];
                row["method_id"]=site.methodId;
                row["method_name"]=SourceUtf8(owner.name);
                row["source"]=SourceUtf8(owner.sourceFile);
            }
            ++boundaryCounts[site.reason];
            j["native_boundaries"].push_back(std::move(row));
        }
        j["native_boundary_counts"]=boundaryCounts;
        j["methods"]=json::array();
        for(const auto& m:p.methods) {
            json row={{"id",m.id},{"name",SourceUtf8(m.name)},{"source",SourceUtf8(m.sourceFile)},
                {"reachable",g.optimization.reachable.contains(m.id)},{"typed",g.typedMethods.contains(m.id)}};
            row["retention_reasons"]=json::array();
            if(auto it=g.optimization.reasons.find(m.id);it!=g.optimization.reasons.end())row["retention_reasons"]=it->second;
            row["calls"]=json::array();
            if(auto it=g.optimization.edges.find(m.id);it!=g.optimization.edges.end())row["calls"]=it->second;
            if(auto it=g.typedRejections.find(m.id);it!=g.typedRejections.end())row["typed_rejection"]=it->second;
            j["methods"].push_back(std::move(row));
        }
        std::ifstream input(executable,std::ios::binary);
        const auto size=std::filesystem::file_size(executable);
        auto read=[&](std::uint64_t offset,void* data,std::size_t bytes){
            if(offset>size||bytes>size-offset)throw std::runtime_error("invalid_pe_bounds");
            input.seekg(static_cast<std::streamoff>(offset));input.read(static_cast<char*>(data),bytes);
            if(!input)throw std::runtime_error("read_pe_failed");
        };
        IMAGE_DOS_HEADER dos{};read(0,&dos,sizeof(dos));
        if(dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<0)throw std::runtime_error("invalid_pe_dos_header");
        DWORD signature=0;read(dos.e_lfanew,&signature,sizeof(signature));
        if(signature!=IMAGE_NT_SIGNATURE)throw std::runtime_error("invalid_pe_signature");
        IMAGE_FILE_HEADER header{};read(std::uint64_t(dos.e_lfanew)+4,&header,sizeof(header));
        std::uint64_t offset=std::uint64_t(dos.e_lfanew)+4+sizeof(header)+header.SizeOfOptionalHeader;
        j["pe_sections"]=json::array();
        for(unsigned i=0;i<header.NumberOfSections;++i) {
            IMAGE_SECTION_HEADER section{};read(offset+std::uint64_t(i)*sizeof(section),&section,sizeof(section));
            char name[9]{};std::memcpy(name,section.Name,8);
            j["pe_sections"].push_back({{"name",name},{"raw_bytes",section.SizeOfRawData},{"virtual_bytes",section.Misc.VirtualSize}});
        }
        if(path.has_parent_path())std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path,std::ios::binary|std::ios::trunc);
        output<<j.dump(2)<<'\n';output.close();
        if(!output)throw std::runtime_error("write_report_failed");
        return true;
    }catch(const std::exception& e){error="optimization_report_failed:"+std::string(e.what());return false;}
}
}
