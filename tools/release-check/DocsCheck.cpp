#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
std::string percentDecode(const std::string &value)
{
    std::string result;
    const auto hex = [](unsigned char ch)
    {
        if (ch >= '0' && ch <= '9')
        {
            return ch - '0';
        }
        ch = static_cast<unsigned char>(std::tolower(ch));
        return ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
    };
    for (size_t i = 0; i < value.size(); ++i)
    {
        if (value[i] == '%' && i + 2 < value.size() && hex(value[i + 1]) >= 0 && hex(value[i + 2]) >= 0)
        {
            result += static_cast<char>(hex(value[i + 1]) * 16 + hex(value[i + 2]));
            i += 2;
        }
        else
        {
            result += value[i];
        }
    }
    return result;
}

std::string unescape(std::string value)
{
    for (const auto &entity : {std::pair{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"},
                             {"&#39;", "'"}, {"&amp;", "&"}})
    {
        size_t start = 0;
        while ((start = value.find(entity.first, start)) != std::string::npos)
        {
            value.replace(start++, std::char_traits<char>::length(entity.first), entity.second);
        }
    }
    return value;
}

std::string slug(std::string text)
{
    static const std::regex tags(R"(<[^>]+>)");
    static const std::regex linkedTitle(R"(\[([^\]]+)\]\([^)]+\))");
    static const std::regex trim(R"(^[ \t]+|[ \t#]+$)");
    text = std::regex_replace(unescape(std::regex_replace(text, tags, "")), linkedTitle, "$1");
    text = std::regex_replace(text, trim, "");
    std::string result;
    for (size_t i = 0; i < text.size();)
    {
        const auto first = static_cast<unsigned char>(text[i]);
        const size_t length = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
        unsigned int code = length == 1 ? first : first & (0x7f >> length);
        if (length > text.size() - i)
        {
            throw std::runtime_error("Truncated UTF-8 in Markdown heading");
        }
        for (size_t j = 1; j < length; ++j)
        {
            code = (code << 6) | (static_cast<unsigned char>(text[i + j]) & 63);
        }
        if (code == ' ')
        {
            result += '-';
        }
        else if (code < 128 && (std::isalnum(first) || first == '-' || first == '_'))
        {
            result += static_cast<char>(std::tolower(first));
        }
        else if (code >= 128 && !(code >= 0x2000 && code <= 0x206f) &&
                 !(code >= 0x3000 && code <= 0x303f) && !(code >= 0xff00 && code <= 0xff65))
        {
            // Keep UTF-8 letters (including Chinese headings), not full-width punctuation.
            result.append(text, i, length);
        }
        i += length;
    }
    return result;
}

struct Page
{
    std::set<std::string> anchors;
    std::set<fs::path> edges;
    std::vector<std::pair<size_t, std::string>> links;
};

Page scan(const fs::path &path)
{
    static const std::regex fence(R"(^[ \t]*(`{3,}|~{3,}))");
    static const std::regex heading(R"(^ {0,3}#{1,6}[ \t]+(.+)$)");
    static const std::regex inlineCode(R"((`+).*?\1)");
    static const std::regex anchor(R"re(\b(?:id|name)=["']([^"']+)["'])re");
    static const std::regex links[] = {
        std::regex(R"(!?\[[^\]]*\]\(\s*(<[^>]+>|[^\s)]+)(?:\s+[^)]*)?\))"),
        std::regex(R"(^ {0,3}\[[^\]]+\]:\s*(<[^>]+>|\S+))"),
        std::regex(R"re(\b(?:href|src)=["']([^"']+)["'])re")};
    Page page;
    std::map<std::string, unsigned int> headings;
    std::ifstream input(path);
    if (!input)
    {
        throw std::runtime_error("Cannot open Markdown: " + path.u8string());
    }
    std::string line, opened;
    size_t number = 0;
    while (std::getline(input, line))
    {
        ++number;
        if (number == 1 && line.compare(0, 3, "\xef\xbb\xbf") == 0)
        {
            line.erase(0, 3);
        }
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        std::smatch match;
        const bool isFence = std::regex_search(line, match, fence);
        if (!opened.empty())
        {
            if (isFence && match[1].str()[0] == opened[0] && match[1].length() >= static_cast<ptrdiff_t>(opened.size()))
            {
                opened.clear();
            }
            continue;
        }
        if (isFence)
        {
            opened = match[1];
            continue;
        }
        for (auto it = std::sregex_iterator(line.begin(), line.end(), anchor); it != std::sregex_iterator(); ++it)
        {
            page.anchors.insert((*it)[1]);
        }
        if (std::regex_search(line, match, heading))
        {
            const auto id = slug(match[1]);
            const auto repeat = headings[id]++;
            page.anchors.insert(repeat ? id + "-" + std::to_string(repeat) : id);
        }
        line = std::regex_replace(line, inlineCode, " ");
        for (const auto &pattern : links)
        {
            for (auto it = std::sregex_iterator(line.begin(), line.end(), pattern); it != std::sregex_iterator(); ++it)
            {
                auto url = (*it)[1].str();
                if (url.size() > 1 && url.front() == '<' && url.back() == '>')
                {
                    url = url.substr(1, url.size() - 2);
                }
                page.links.emplace_back(number, unescape(std::move(url)));
            }
        }
    }
    return page;
}
} // namespace

int checkDocs(const fs::path &directory)
{
    const auto root = fs::canonical(directory);
    const auto readme = root / "README.md";
    const auto index = root / "docs" / "README.md";
    std::map<fs::path, Page> pages{{readme, scan(readme)}};
    for (const auto &entry : fs::recursive_directory_iterator(root / "docs"))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".md")
        {
            pages.emplace(entry.path(), scan(entry.path()));
        }
    }
    size_t count = 0, errors = 0;
    static const std::regex remote(R"(^([a-zA-Z][a-zA-Z0-9+.-]*:|//))");
    for (auto &[path, page] : pages)
    {
        for (const auto &[line, url] : page.links)
        {
            if (std::regex_search(url, remote))
            {
                continue;
            }
            ++count;
            const auto hash = url.find('#');
            auto relative = url.substr(0, hash);
            relative = percentDecode(relative.substr(0, relative.find('?')));
            const auto target = relative.empty() ? path : fs::weakly_canonical(path.parent_path() / fs::u8path(relative));
            const auto within = target.lexically_relative(root);
            const auto destination = pages.find(target);
            std::string error;
            if (within.empty() || *within.begin() == "..")
            {
                error = "link escapes documentation root";
            }
            else if (!fs::exists(target))
            {
                error = "missing target";
            }
            else if (hash != std::string::npos && hash + 1 < url.size() && destination != pages.end() &&
                     !destination->second.anchors.count(percentDecode(url.substr(hash + 1))))
            {
                error = "missing anchor";
            }
            if (!error.empty())
            {
                ++errors;
                std::cerr << path.lexically_relative(root).u8string() << ':' << line << ": " << error << ": " << url << '\n';
            }
            if (destination != pages.end())
            {
                page.edges.insert(target);
            }
        }
    }
    std::set<fs::path> visited;
    std::vector<fs::path> queue{index};
    if (!pages.count(index))
    {
        ++errors;
        std::cerr << "Missing documentation index: docs/README.md\n";
    }
    while (!queue.empty())
    {
        const auto path = std::move(queue.back());
        queue.pop_back();
        if (const auto page = pages.find(path); visited.insert(path).second && page != pages.end())
        {
            queue.insert(queue.end(), page->second.edges.begin(), page->second.edges.end());
        }
    }
    for (const auto &entry : pages)
    {
        if (entry.first != readme && !visited.count(entry.first))
        {
            ++errors;
            std::cerr << "Not reachable from docs/README.md: " << entry.first.lexically_relative(root).u8string() << '\n';
        }
    }
    std::cout << "Documentation: " << pages.size() << " pages, " << count << " local links, " << errors << " errors\n";
    return errors ? 1 : 0;
}
