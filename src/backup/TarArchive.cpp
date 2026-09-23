#include "abp/TarArchive.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <stdexcept>

namespace abp::tar {
namespace {

constexpr size_t kBlock = 512;

std::string field(const std::array<char, kBlock>& header, size_t offset, size_t length) {
    size_t end = offset;
    while (end < offset + length && header[end] != '\0') ++end;
    return std::string(header.data() + offset, end - offset);
}

/// Numeric header fields are octal text, except that GNU tar switches to
/// big-endian base-256 (high bit of the first byte set) for values that do
/// not fit -- files over 8 GiB, which a phone's video library can contain.
unsigned long long number(const std::array<char, kBlock>& header, size_t offset, size_t length) {
    const auto first = static_cast<unsigned char>(header[offset]);
    if (first & 0x80) {
        unsigned long long value = first & 0x7F;
        for (size_t i = offset + 1; i < offset + length; ++i) {
            value = (value << 8) | static_cast<unsigned char>(header[i]);
        }
        return value;
    }
    unsigned long long value = 0;
    for (size_t i = offset; i < offset + length; ++i) {
        const char c = header[i];
        if (c == ' ' && value == 0) continue; // Leading padding.
        if (c < '0' || c > '7') break;
        value = value * 8 + static_cast<unsigned long long>(c - '0');
    }
    return value;
}

bool isZeroBlock(const std::array<char, kBlock>& header) {
    return std::all_of(header.begin(), header.end(), [](char c) { return c == '\0'; });
}

bool checksumValid(const std::array<char, kBlock>& header) {
    unsigned long long sum = 0;
    for (size_t i = 0; i < kBlock; ++i) {
        // The checksum field itself counts as eight spaces.
        sum += (i >= 148 && i < 156) ? static_cast<unsigned long long>(' ') : static_cast<unsigned char>(header[i]);
    }
    return sum == number(header, 148, 8);
}

std::string cleanName(std::string name) {
    while (name.rfind("./", 0) == 0) name.erase(0, 2);
    while (!name.empty() && name.front() == '/') name.erase(0, 1);
    return name;
}

/// Parses pax extended header records ("<len> key=value\n") into `out`.
void parsePax(const std::string& data, std::map<std::string, std::string>& out) {
    size_t pos = 0;
    while (pos < data.size()) {
        const size_t space = data.find(' ', pos);
        if (space == std::string::npos) break;
        size_t length = 0;
        try {
            length = static_cast<size_t>(std::stoul(data.substr(pos, space - pos)));
        } catch (const std::exception&) {
            break;
        }
        if (length == 0 || pos + length > data.size()) break;
        const std::string record = data.substr(space + 1, length - (space - pos) - 2); // Drop the trailing '\n'.
        const size_t equals = record.find('=');
        if (equals != std::string::npos) out[record.substr(0, equals)] = record.substr(equals + 1);
        pos += length;
    }
}

} // namespace

std::vector<Entry> index(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open archive: " + path.string());

    std::vector<Entry> entries;
    std::array<char, kBlock> header{};
    unsigned long long position = 0;
    std::string longName;
    std::string longLink;
    std::map<std::string, std::string> pax;

    auto readData = [&](unsigned long long size) {
        std::string data(static_cast<size_t>(size), '\0');
        in.read(data.data(), static_cast<std::streamsize>(size));
        if (!in) throw std::runtime_error("truncated archive: " + path.string());
        return data;
    };
    auto skipTo = [&](unsigned long long target) {
        in.seekg(static_cast<std::streamoff>(target));
        position = target;
    };

    while (true) {
        in.read(header.data(), kBlock);
        if (in.gcount() == 0) break; // End of file without the end-of-archive blocks: tolerated.
        if (in.gcount() != static_cast<std::streamsize>(kBlock)) {
            throw std::runtime_error("truncated archive: " + path.string());
        }
        position += kBlock;
        if (isZeroBlock(header)) break;
        if (!checksumValid(header)) {
            if (entries.empty()) throw std::runtime_error("not a tar archive: " + path.string());
            throw std::runtime_error("corrupt tar header at offset " + std::to_string(position - kBlock) + " in " +
                                     path.string());
        }

        const char type = header[156];
        unsigned long long size = number(header, 124, 12);
        const unsigned long long dataOffset = position;
        const unsigned long long next = dataOffset + (size + kBlock - 1) / kBlock * kBlock;

        if (type == 'L' || type == 'K' || type == 'x') {
            // Metadata for the entry that follows, not an entry itself.
            skipTo(dataOffset);
            std::string data = readData(size);
            if (type == 'L') longName = data.c_str();
            else if (type == 'K') longLink = data.c_str();
            else parsePax(data, pax);
            skipTo(next);
            continue;
        }
        if (type == 'g') { // Global pax header: nothing abp needs.
            skipTo(next);
            continue;
        }

        Entry entry;
        std::string name = field(header, 0, 100);
        const std::string prefix = field(header, 345, 155);
        if (field(header, 257, 5) == "ustar" && !prefix.empty()) name = prefix + "/" + name;
        if (!longName.empty()) name = longName;
        if (pax.count("path")) name = pax["path"];
        if (pax.count("size")) {
            try {
                size = std::stoull(pax["size"]);
            } catch (const std::exception&) {
            }
        }
        entry.name = cleanName(name);
        entry.linkTarget = !longLink.empty() ? longLink : pax.count("linkpath") ? pax["linkpath"] : field(header, 157, 100);
        entry.mtime = static_cast<long long>(number(header, 136, 12));
        entry.offset = dataOffset;
        switch (type) {
            case '0': case '\0': case '7': entry.type = Entry::Type::File; break;
            case '5': entry.type = Entry::Type::Directory; break;
            case '2': entry.type = Entry::Type::Symlink; break;
            default: entry.type = Entry::Type::Other; break;
        }
        if (entry.type == Entry::Type::File && !entry.name.empty() && entry.name.back() == '/') {
            entry.type = Entry::Type::Directory; // Pre-POSIX tars mark directories only by the slash.
        }
        entry.size = entry.type == Entry::Type::File ? size : 0;
        if (entry.type == Entry::Type::Directory) {
            while (!entry.name.empty() && entry.name.back() == '/') entry.name.pop_back();
        }
        if (!entry.name.empty()) entries.push_back(std::move(entry));

        longName.clear();
        longLink.clear();
        pax.clear();
        // Links carry no data even when a size is recorded.
        skipTo(type == '1' || type == '2' || type == '5' ? dataOffset : next);
    }
    return entries;
}

bool isGzip(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    unsigned char magic[2] = {0, 0};
    in.read(reinterpret_cast<char*>(magic), 2);
    return in.gcount() == 2 && magic[0] == 0x1f && magic[1] == 0x8b;
}

std::vector<Child> listChildren(const std::vector<Entry>& entries, const std::string& prefix) {
    std::map<std::string, Child> children;
    for (const auto& entry : entries) {
        if (entry.name.size() <= prefix.size() || entry.name.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string rest = entry.name.substr(prefix.size());
        const size_t slash = rest.find('/');
        const std::string name = rest.substr(0, slash);
        Child& child = children[name];
        child.name = name;
        child.path = prefix + name;
        if (slash != std::string::npos || entry.type == Entry::Type::Directory) {
            child.isDirectory = true;
            child.size += entry.size;
        } else {
            child.size = entry.size;
        }
        child.mtime = std::max(child.mtime, entry.mtime);
    }

    std::vector<Child> result;
    result.reserve(children.size());
    for (auto& [name, child] : children) result.push_back(std::move(child));
    std::stable_sort(result.begin(), result.end(),
                     [](const Child& a, const Child& b) { return a.isDirectory && !b.isDirectory; });
    return result;
}

} // namespace abp::tar
