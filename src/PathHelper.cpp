
#include "PathHelper.h"
#include <optional>
#include <vector>
#include <regex>
#include <fstream>
#include <algorithm>
#include <regex>
#include <filesystem>


std::string GetBasePath() {
    char buffer[MAX_PATH];
    GetModuleFileName(NULL, buffer, MAX_PATH);
    std::string::size_type pos = std::string(buffer).find_last_of("\\/");
    return std::string(buffer).substr(0, pos);
}

std::string ExtractBetweenDashes(const std::string& text) {
    std::string delimiter = " - ";

    // �ҵ���һ��" - "��λ��
    size_t start = text.find(delimiter);
    if (start == std::string::npos) {
        // û���ҵ�" - "�����ؿ��ַ���
        return "";
    }
    start += delimiter.length(); // ����" - "����ʼ�ڵ�һ��" - "֮����ַ�

    // ��startλ�ÿ�ʼ���ҵ���һ��" - "��λ��
    size_t end = text.find(delimiter, start);
    if (end == std::string::npos) {
        // û���ҵ��ڶ���" - "�����ؿ��ַ���
        return "";
    }

    // ȡ������" - "֮������ַ���
    return text.substr(start, end - start);
}

/// <summary>
/// ��ȡ
/// </summary>
/// <param name="filename"></param>
/// <param name="search_bytes"></param>
/// <returns></returns>
std::optional<size_t> FindByteInFile(const std::string& filename, const std::vector<char>& search_bytes) {
    // ���ļ�����ȡ����
    std::ifstream file(filename, std::ios::binary);
    std::vector<char> file_contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    // �����ֽ�����
    auto it = std::search(file_contents.begin(), file_contents.end(), search_bytes.begin(), search_bytes.end());

    if (it != file_contents.end()) {
        // ����ҵ��ˣ�����λ��
        return std::distance(file_contents.begin(), it);
    }
    else {
        // ���û���ҵ������� std::nullopt
        return std::nullopt;
    }
}




/// <summary>
/// ��ȡout���ļ���
/// </summary>
/// <param name="s"></param>
/// <returns></returns>
std::string GetLinkerCommandOutFileName(const std::string& s) {
    std::regex reg("/out:\"([^\"]*)\"");  
    std::smatch match;

    if (std::regex_search(s, match, reg) && match.size() > 1) {
        std::string path = match.str(1); 
        std::filesystem::path fs_path(path);
        return fs_path.filename().string(); 
    }
    else {
        return ""; 
    }
}



// ���������������ı�����ȡ�����ض�������·��
std::string ExtractPathFromCommand(const std::string& commandLine, const std::string& target) {
    std::string foundPath;
    size_t pos = commandLine.find(target);
    if (pos != std::string::npos) {
        // �������ַ�����λ����ǰ���ҵ�һ��˫����
        size_t start = commandLine.rfind('"', pos);
        // �������ַ�����λ�������ҵ�һ��˫����
        size_t end = commandLine.find('"', pos + target.length());

        // ����ҵ���˫���ţ���ȡ·��
        if (start != std::string::npos && end != std::string::npos) {
            foundPath = commandLine.substr(start + 1, end - start - 1);
        }
    }
    return foundPath;
}

std::string GetLinkerCommandKrnlnFileName(const std::string& s) {
    std::string target = "\\static_lib\\krnln_static.lib";
    std::string foundPath = ExtractPathFromCommand(s, target);
    return foundPath;
}

