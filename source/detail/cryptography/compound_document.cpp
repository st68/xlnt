// Copyright (C) 2016-2017 Thomas Fussell
// Copyright (C) 2002-2007 Ariya Hidayat (ariya@kde.org).
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
// IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
// OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
// IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
// NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
// THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 
#include <array>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <locale>
#include <string>
#include <vector>

#include <detail/binary.hpp>
#include <detail/unicode.hpp>
#include <detail/cryptography/compound_document.hpp>
#include <xlnt/utils/exceptions.hpp>

namespace {

using namespace xlnt::detail;

int compare_keys(const std::string &left, const std::string &right)
{
    auto to_lower = [](std::string s)
    {
        static const auto locale = std::locale();
        std::use_facet<std::ctype<char>>(locale).tolower(&s[0], &s[0] + s.size());
        return s;
    };

    return to_lower(left).compare(to_lower(right));
}

std::string join_path(const std::vector<std::string> &path)
{
    auto joined = std::string();

    for (auto part : path)
    {
        joined.append(part);
        joined.push_back('/');
    }

    return joined;
}

const sector_id FreeSector = -1;
const sector_id EndOfChain = -2;
const sector_id SATSector = -3;
//const sector_id MSATSector = -4;

const directory_id End = -1;

} // namespace

namespace xlnt {
namespace detail {

/// <summary>
/// Allows a std::vector to be read through a std::istream.
/// </summary>
class compound_document_istreambuf : public std::streambuf
{
    using int_type = std::streambuf::int_type;

public:
    compound_document_istreambuf(const std::string &filename)
        : data_(filename.begin(), filename.end()),
        position_(0)
    {
    }

    compound_document_istreambuf(const compound_document_istreambuf &) = delete;
    compound_document_istreambuf &operator=(const compound_document_istreambuf &) = delete;

private:
    int_type underflow()
    {
        if (position_ == data_.size())
        {
            return traits_type::eof();
        }

        return traits_type::to_int_type(static_cast<char>(data_[position_]));
    }

    int_type uflow()
    {
        if (position_ == data_.size())
        {
            return traits_type::eof();
        }

        return traits_type::to_int_type(static_cast<char>(data_[position_++]));
    }

    std::streamsize showmanyc()
    {
        if (position_ == data_.size())
        {
            return static_cast<std::streamsize>(-1);
        }

        return static_cast<std::streamsize>(data_.size() - position_);
    }

    std::streampos seekoff(std::streamoff off, std::ios_base::seekdir way, std::ios_base::openmode)
    {
        if (way == std::ios_base::beg)
        {
            position_ = 0;
        }
        else if (way == std::ios_base::end)
        {
            position_ = data_.size();
        }

        if (off < 0)
        {
            if (static_cast<std::size_t>(-off) > position_)
            {
                position_ = 0;
                return static_cast<std::ptrdiff_t>(-1);
            }
            else
            {
                position_ -= static_cast<std::size_t>(-off);
            }
        }
        else if (off > 0)
        {
            if (static_cast<std::size_t>(off) + position_ > data_.size())
            {
                position_ = data_.size();
                return static_cast<std::ptrdiff_t>(-1);
            }
            else
            {
                position_ += static_cast<std::size_t>(off);
            }
        }

        return static_cast<std::ptrdiff_t>(position_);
    }

    std::streampos seekpos(std::streampos sp, std::ios_base::openmode)
    {
        if (sp < 0)
        {
            position_ = 0;
        }
        else if (static_cast<std::size_t>(sp) > data_.size())
        {
            position_ = data_.size();
        }
        else
        {
            position_ = static_cast<std::size_t>(sp);
        }

        return static_cast<std::ptrdiff_t>(position_);
    }

private:
    std::vector<std::uint8_t> data_;
    std::size_t position_;
};

/// <summary>
/// Allows a std::vector to be written through a std::ostream.
/// </summary>
class compound_document_ostreambuf : public std::streambuf
{
    using int_type = std::streambuf::int_type;

public:
    compound_document_ostreambuf(const std::string &filename)
        : data_(filename.begin(), filename.end()),
        position_(0)
    {
    }

    compound_document_ostreambuf(const compound_document_ostreambuf &) = delete;
    compound_document_ostreambuf &operator=(const compound_document_ostreambuf &) = delete;

private:
    int_type overflow(int_type c = traits_type::eof())
    {
        if (c != traits_type::eof())
        {
            data_.push_back(static_cast<std::uint8_t>(c));
            position_ = data_.size() - 1;
        }

        return traits_type::to_int_type(static_cast<char>(data_[position_]));
    }

    std::streamsize xsputn(const char *s, std::streamsize n)
    {
        if (data_.empty())
        {
            data_.resize(static_cast<std::size_t>(n));
        }
        else
        {
            auto position_size = data_.size();
            auto required_size = static_cast<std::size_t>(position_ + static_cast<std::size_t>(n));
            data_.resize(std::max(position_size, required_size));
        }

        std::copy(s, s + n, data_.begin() + static_cast<std::ptrdiff_t>(position_));
        position_ += static_cast<std::size_t>(n);

        return n;
    }

    std::streampos seekoff(std::streamoff off, std::ios_base::seekdir way, std::ios_base::openmode)
    {
        if (way == std::ios_base::beg)
        {
            position_ = 0;
        }
        else if (way == std::ios_base::end)
        {
            position_ = data_.size();
        }

        if (off < 0)
        {
            if (static_cast<std::size_t>(-off) > position_)
            {
                position_ = 0;
                return static_cast<std::ptrdiff_t>(-1);
            }
            else
            {
                position_ -= static_cast<std::size_t>(-off);
            }
        }
        else if (off > 0)
        {
            if (static_cast<std::size_t>(off) + position_ > data_.size())
            {
                position_ = data_.size();
                return static_cast<std::ptrdiff_t>(-1);
            }
            else
            {
                position_ += static_cast<std::size_t>(off);
            }
        }

        return static_cast<std::ptrdiff_t>(position_);
    }

    std::streampos seekpos(std::streampos sp, std::ios_base::openmode)
    {
        if (sp < 0)
        {
            position_ = 0;
        }
        else if (static_cast<std::size_t>(sp) > data_.size())
        {
            position_ = data_.size();
        }
        else
        {
            position_ = static_cast<std::size_t>(sp);
        }

        return static_cast<std::ptrdiff_t>(position_);
    }

private:
    std::vector<std::uint8_t> data_;
    std::size_t position_;
};


compound_document::compound_document(std::ostream &out)
    : out_(&out),
      stream_in_(nullptr),
      stream_out_(nullptr)
{
    write_header();
    insert_entry("Root Entry", compound_document_entry::entry_type::RootStorage);
}

compound_document::compound_document(std::istream &in)
    : in_(&in),
      stream_in_(nullptr),
      stream_out_(nullptr)
{
    read_header();
    read_msat();
    read_sat();
    read_ssat();
    read_directory();
}

compound_document::~compound_document()
{
}

std::size_t compound_document::sector_size()
{
    return static_cast<std::size_t>(1) << header_.sector_size_power;
}

std::size_t compound_document::short_sector_size()
{
    return static_cast<std::size_t>(1) << header_.short_sector_size_power;
}

std::istream &compound_document::open_read_stream(const std::string &name)
{
    const auto entry_id = find_entry(name, compound_document_entry::entry_type::UserStream);
    const auto &entry = entries_.at(entry_id);

    stream_in_buffer_.reset(new compound_document_istreambuf(name));
    stream_in_.rdbuf(stream_in_buffer_.get());

    return stream_in_;
}

std::ostream &compound_document::open_write_stream(const std::string &name)
{
    auto entry_id = contains_entry(name, compound_document_entry::entry_type::UserStream)
        ? find_entry(name, compound_document_entry::entry_type::UserStream)
        : insert_entry(name, compound_document_entry::entry_type::UserStream);
    auto &entry = entries_.at(entry_id);

    stream_out_buffer_.reset(new compound_document_ostreambuf(name));
    stream_out_.rdbuf(stream_out_buffer_.get());

    return stream_out_;
}

template<typename T>
void compound_document::write_sector(binary_reader<T> &reader, sector_id id)
{
    out_->seekp(sector_data_start() + sector_size() * id);
    out_->write(reinterpret_cast<const char *>(reader.data() + reader.offset()),
        std::min(sector_size(), reader.bytes() - reader.offset()));
}

template<typename T>
void compound_document::write_short_sector(binary_reader<T> &reader, sector_id id)
{
    auto chain = follow_chain(entries_[0].start, sat_);
    auto sector_id = chain[id / (sector_size() / short_sector_size())];
    auto sector_offset = id % (sector_size() / short_sector_size()) * short_sector_size();
    out_->seekp(sector_data_start() + sector_size() * sector_id + sector_offset);
    out_->write(reinterpret_cast<const char *>(reader.data() + reader.offset()),
        std::min(short_sector_size(), reader.bytes() - reader.offset()));
}

template<typename T>
void compound_document::read_sector(sector_id id, binary_writer<T> &writer)
{
    in_->seekg(sector_data_start() + sector_size() * id);
    std::vector<byte> sector(sector_size(), 0);
    in_->read(reinterpret_cast<char *>(sector.data()), sector_size());
    writer.append(sector);
}

template<typename T>
void compound_document::read_short_sector(sector_id id, binary_writer<T> &writer)
{
    const auto container_chain = follow_chain(entries_[0].start, sat_);
    auto container = std::vector<byte>();
    auto container_writer = binary_writer<byte>(container);
    
    for (auto sector : container_chain)
    {
        read_sector(sector, container_writer);
    }

    auto container_reader = binary_reader<byte>(container);
    container_reader.offset(id * short_sector_size());

    writer.append(container_reader, short_sector_size());
}

sector_id compound_document::allocate_sector()
{
    const auto sectors_per_sector = sector_size() / sizeof(sector_id);
    auto next_free_iter = std::find(sat_.begin(), sat_.end(), FreeSector);
    
    if (next_free_iter == sat_.end())
    {
        auto next_msat_index = header_.num_msat_sectors;
        auto new_sat_sector_id = sector_id(sat_.size());
        msat_.push_back(new_sat_sector_id);
        header_.msat[msat_.size() - 1] = new_sat_sector_id;
        sat_.resize(sat_.size() + sectors_per_sector, FreeSector);
        sat_[new_sat_sector_id] = SATSector;
        auto sat_reader = binary_reader<sector_id>(sat_);
        sat_reader.offset(next_msat_index * sectors_per_sector);
        write_sector(sat_reader, new_sat_sector_id);
        next_free_iter = std::find(sat_.begin(), sat_.end(), FreeSector);
    }
    
    auto next_free = sector_id(next_free_iter - sat_.begin());
    sat_[next_free] = EndOfChain;

    write_sat();
    
    auto empty_sector = std::vector<byte>(sector_size());
    auto empty_sector_reader = binary_reader<byte>(empty_sector);
    write_sector(empty_sector_reader, next_free);
    
    return next_free;
}

sector_chain compound_document::allocate_sectors(std::size_t count)
{
    if (count == std::size_t(0)) return {};

    auto chain = sector_chain();
    auto current = allocate_sector();

    for (auto i = std::size_t(1); i < count; ++i)
    {
        chain.push_back(current);
        auto next = allocate_sector();
        sat_[current] = next;
        current = next;
    }

    return chain;
}

sector_chain compound_document::follow_chain(sector_id start, const sector_chain &table)
{
    auto chain = sector_chain();
    auto current = start;

    while (current >= 0)
    {
        chain.push_back(current);
        current = table[current];
    }

    return chain;
}

sector_chain compound_document::allocate_short_sectors(std::size_t count)
{
    if (count == std::size_t(0)) return {};

    auto chain = sector_chain();
    auto current = allocate_short_sector();

    for (auto i = std::size_t(1); i < count; ++i)
    {
        chain.push_back(current);
        auto next = allocate_short_sector();
        ssat_[current] = next;
        current = next;
    }
    
    chain.push_back(current);

    return chain;
}

sector_id compound_document::allocate_short_sector()
{
    const auto sectors_per_sector = sector_size() / sizeof(sector_id);
    auto next_free_iter = std::find(ssat_.begin(), ssat_.end(), FreeSector);
    
    if (next_free_iter == ssat_.end())
    {
        auto new_ssat_sector_id = allocate_sector();
        
        ++header_.num_short_sectors;
        
        if (header_.ssat_start < 0)
        {
            header_.ssat_start = new_ssat_sector_id;
        }
        else
        {
            auto ssat_chain = follow_chain(header_.ssat_start, sat_);
            sat_[ssat_chain.back()] = new_ssat_sector_id;
        }
        
        write_header();
        
        auto old_size = ssat_.size();
        ssat_.resize(old_size + sectors_per_sector, FreeSector);

        auto ssat_reader = binary_reader<sector_id>(ssat_);
        ssat_reader.offset(old_size / sectors_per_sector);
        write_sector(ssat_reader, new_ssat_sector_id);

        next_free_iter = std::find(ssat_.begin(), ssat_.end(), FreeSector);
    }
    
    auto next_free = sector_id(next_free_iter - ssat_.begin());
    ssat_[next_free] = EndOfChain;

    write_ssat();
    
    const auto short_sectors_per_sector = sector_size() / short_sector_size();
    const auto required_container_sectors = std::size_t(next_free / short_sectors_per_sector + 1);
    
    if (required_container_sectors > 0)
    {
        if (entries_[0].start < 0)
        {
            entries_[0].start = allocate_sector();
            write_entry(0);
        }
        
        auto container_chain = follow_chain(entries_[0].start, sat_);
        
        if (required_container_sectors > container_chain.size())
        {
            sat_[container_chain.back()] = allocate_sector();
            write_sat();
        }
    }
    
    return next_free;
}

directory_id compound_document::next_empty_entry()
{
    auto entry_id = directory_id(0);

    for (; entry_id < directory_id(entries_.size()); ++entry_id)
    {
        auto &entry = entries_[entry_id];

        if (entry.type == compound_document_entry::entry_type::Empty)
        {
            return entry_id;
        }
    }

    // entry_id is now equal to entries_.size()

    const auto entries_per_sector = sector_size() 
        / sizeof(compound_document_entry);
    auto new_sector = allocate_sector();
    // TODO: connect chains here

    for (auto i = std::size_t(0); i < entries_per_sector; ++i)
    {
        auto empty_entry = compound_document_entry();
        empty_entry.type = compound_document_entry::entry_type::Empty;
        entries_.push_back(empty_entry);
        write_entry(entry_id + directory_id(i));
    }

    return entry_id;
}

directory_id compound_document::insert_entry(
    const std::string &name,
    compound_document_entry::entry_type type)
{
    auto entry_id = next_empty_entry();
    auto &entry = entries_[entry_id];

    entry.name(name);
    entry.type = type;

    write_entry(entry_id);

    // TODO: parse path from name and use correct parent storage instead of 0
    tree_insert(entry_id, 0);

    return entry_id;
}

std::size_t compound_document::sector_data_start()
{
    return sizeof(compound_document_header);
}

bool compound_document::contains_entry(const std::string &path,
        compound_document_entry::entry_type type)
{
    return find_entry(path, type) >= 0;
}

directory_id compound_document::find_entry(const std::string &name,
    compound_document_entry::entry_type type)
{
    if (type == compound_document_entry::entry_type::RootStorage
        && (name == "/" || name == "/Root Entry")) return 0;

    auto entry_id = directory_id(0);

    for (auto &entry : entries_)
    {
        if (entry.type == type && tree_path(entry_id) == name)
        {
            return entry_id;
        }

        ++entry_id;
    }

    return End;
}

void compound_document::print_directory()
{
    auto entry_id = directory_id(0);

    for (auto &entry : entries_)
    {
        if (entry.type == compound_document_entry::entry_type::UserStream)
        {
            std::cout << tree_path(entry_id) << std::endl;
        }

        ++entry_id;
    }
}

void compound_document::read_directory()
{
    const auto entries_per_sector = sector_size() / sizeof(compound_document_entry);
    auto entry_id = directory_id(0);

    for (auto sector : follow_chain(header_.directory_start, sat_))
    {
        for (auto i = std::size_t(0); i < entries_per_sector; ++i)
        {
            entries_.push_back(compound_document_entry());
            read_entry(entry_id++);
        }
    }

    auto stack = std::vector<directory_id>();
    auto storage_siblings = std::vector<directory_id>();
    auto stream_siblings = std::vector<directory_id>();

    auto directory_stack = std::vector<directory_id>();
    directory_stack.push_back(directory_id(0));

    while (!directory_stack.empty())
    {
        auto current_storage_id = directory_stack.back();
        directory_stack.pop_back();

        if (tree_child(current_storage_id) < 0) continue;

        auto storage_stack = std::vector<directory_id>();
        auto storage_root_id = tree_child(current_storage_id);
        parent_[storage_root_id] = End;
        storage_stack.push_back(storage_root_id);

        while (!storage_stack.empty())
        {
            auto current_entry_id = storage_stack.back();
            auto current_entry = entries_[current_entry_id];
            storage_stack.pop_back();

            parent_storage_[current_entry_id] = current_storage_id;

            if (current_entry.type == compound_document_entry::entry_type::UserStorage)
            {
                directory_stack.push_back(current_entry_id);
            }

            if (tree_left(current_entry_id) >= 0)
            {
                storage_stack.push_back(tree_left(current_entry_id));
                tree_parent(tree_left(current_entry_id)) = current_entry_id;
            }

            if (tree_right(current_entry_id) >= 0)
            {
                storage_stack.push_back(tree_right(current_entry_id));
                tree_parent(tree_right(current_entry_id)) = current_entry_id;
            }
        }
    }
}

void compound_document::tree_insert(directory_id new_id, directory_id storage_id)
{
    using entry_color = compound_document_entry::entry_color;

    parent_storage_[new_id] = storage_id;

    tree_left(new_id) = End;
    tree_right(new_id) = End;

    if (tree_root(new_id) == End)
    {
        if (new_id != 0)
        {
            tree_root(new_id) = new_id;
        }

        tree_color(new_id) = entry_color::Black;
        tree_parent(new_id) = End;

        return;
    }

    // normal tree insert
    // (will probably unbalance the tree, fix after)
    auto x = tree_root(new_id);
    auto y = End;

    while (x >= 0)
    {
        y = x;

        if (compare_keys(tree_key(new_id), tree_key(x)) > 0)
        {
            x = tree_right(x);
        }
        else
        {
            x = tree_left(x);
        }
    }

    tree_parent(new_id) = y;

    if (compare_keys(tree_key(new_id), tree_key(y)) > 0)
    {
        tree_right(y) = new_id;
    }
    else
    {
        tree_left(y) = new_id;
    }

    tree_insert_fixup(new_id);
}

std::string compound_document::tree_path(directory_id id)
{
    auto storage_id = parent_storage_[id];
    auto result = std::vector<std::string>();

    while (storage_id > 0)
    {
        storage_id = parent_storage_[storage_id];
        result.push_back(entries_[storage_id].name());
    }

    return "/" + join_path(result) + entries_[id].name();
}

void compound_document::tree_rotate_left(directory_id x)
{
    auto y = tree_right(x);

    // turn y's left subtree into x's right subtree
    tree_right(x) = tree_left(y);

    if (tree_left(y) != End)
    {
        tree_parent(tree_left(y)) = x;
    }

    // link x's parent to y
    tree_parent(y) = tree_parent(x);

    if (tree_parent(x) == End)
    {
        tree_root(x) = y;
    }
    else if (x == tree_left(tree_parent(x)))
    {
        tree_left(tree_parent(x)) = y;
    }
    else
    {
        tree_right(tree_parent(x)) = y;
    }

    // put x on y's left
    tree_left(y) = x;
    tree_parent(x) = y;
}

void compound_document::tree_rotate_right(directory_id y)
{
    auto x = tree_left(y);

    // turn x's right subtree into y's left subtree
    tree_left(y) = tree_right(x);

    if (tree_right(x) != End)
    {
        tree_parent(tree_right(x)) = y;
    }

    // link y's parent to x
    tree_parent(x) = tree_parent(y);

    if (tree_parent(y) == End)
    {
        tree_root(y) = x;
    }
    else if (y == tree_left(tree_parent(y)))
    {
        tree_left(tree_parent(y)) = x;
    }
    else
    {
        tree_right(tree_parent(y)) = x;
    }

    // put y on x's right
    tree_right(x) = y;
    tree_parent(y) = x;
}

void compound_document::tree_insert_fixup(directory_id x)
{
    using entry_color = compound_document_entry::entry_color;

    tree_color(x) = entry_color::Red;

    while (x != tree_root(x) && tree_color(tree_parent(x)) == entry_color::Red)
    {
        if (tree_parent(x) == tree_left(tree_parent(tree_parent(x))))
        {
            auto y = tree_right(tree_parent(tree_parent(x)));

            if (y >= 0 && tree_color(y) == entry_color::Red)
            {
                // case 1
                tree_color(tree_parent(x)) = entry_color::Black;
                tree_color(y) = entry_color::Black;
                tree_color(tree_parent(tree_parent(x))) = entry_color::Red;
                x = tree_parent(tree_parent(x));
            }
            else
            {
                if (x == tree_right(tree_parent(x)))
                {
                    // case 2
                    x = tree_parent(x);
                    tree_rotate_left(x);
                }

                // case 3
                tree_color(tree_parent(x)) = entry_color::Black;
                tree_color(tree_parent(tree_parent(x))) = entry_color::Red;
                tree_rotate_right(tree_parent(tree_parent(x)));
            }
        }
        else // same as above with left and right switched
        {
            auto y = tree_left(tree_parent(tree_parent(x)));

            if (y >= 0 && tree_color(y) == entry_color::Red)
            {
                //case 1
                tree_color(tree_parent(x)) = entry_color::Black;
                tree_color(y) = entry_color::Black;
                tree_color(tree_parent(tree_parent(x))) = entry_color::Red;
                x = tree_parent(tree_parent(x));
            }
            else
            {
                if (x == tree_left(tree_parent(x)))
                {
                    // case 2
                    x = tree_parent(x);
                    tree_rotate_right(x);
                }

                // case 3
                tree_color(tree_parent(x)) = entry_color::Black;
                tree_color(tree_parent(tree_parent(x))) = entry_color::Red;
                tree_rotate_left(tree_parent(tree_parent(x)));
            }
        }
    }

    tree_color(tree_root(x)) = entry_color::Black;
}

directory_id &compound_document::tree_left(directory_id id)
{
    return entries_[id].prev;
}

directory_id &compound_document::tree_right(directory_id id)
{
    return entries_[id].next;
}

directory_id &compound_document::tree_parent(directory_id id)
{
    return parent_[id];
}

directory_id &compound_document::tree_root(directory_id id)
{
    return tree_child(parent_storage_[id]);
}

directory_id &compound_document::tree_child(directory_id id)
{
    return entries_[id].child;
}

std::string compound_document::tree_key(directory_id id)
{
    return entries_[id].name();
}

compound_document_entry::entry_color &compound_document::tree_color(directory_id id)
{
    return entries_[id].color;
}

void compound_document::read_header()
{
    in_->seekg(0, std::ios::beg);
    in_->read(reinterpret_cast<char *>(&header_), sizeof(compound_document_header));
}

void compound_document::read_msat()
{
    msat_.clear();

    auto msat_sector = header_.extra_msat_start;
    auto msat_writer = binary_writer<sector_id>(msat_);

    for (auto i = std::uint32_t(0); i < header_.num_msat_sectors; ++i)
    {
        if (i < std::uint32_t(109))
        {
            msat_writer.write(header_.msat[i]);
        }
        else
        {
            read_sector(msat_sector, msat_writer);

            msat_sector = msat_.back();
            msat_.pop_back();
        }
    }
}

void compound_document::read_sat()
{
    sat_.clear();

    auto sat_writer = binary_writer<sector_id>(sat_);

    for (auto msat_sector : msat_)
    {
        read_sector(msat_sector, sat_writer);
    }
}

void compound_document::read_ssat()
{
    ssat_.clear();

    for (auto ssat_sector : follow_chain(header_.ssat_start, sat_))
    {
        auto sector = std::vector<sector_id>();
        auto sector_writer = binary_writer<sector_id>(sector);

        read_sector(ssat_sector, sector_writer);

        std::copy(sector.begin(), sector.end(), std::back_inserter(ssat_));
    }
}

void compound_document::read_entry(directory_id id)
{
    const auto directory_chain = follow_chain(header_.directory_start, sat_);
    const auto entries_per_sector = sector_size() / sizeof(compound_document_entry);
    const auto directory_sector = directory_chain[id / entries_per_sector];
    const auto offset = sector_size() * directory_sector
        + ((id % entries_per_sector) * sizeof(compound_document_entry));

    in_->seekg(sector_data_start() + offset, std::ios::beg);
    in_->read(reinterpret_cast<char *>(&entries_[id]), sizeof(compound_document_entry));
}

void compound_document::write_header()
{
    out_->seekp(0, std::ios::beg);
    out_->write(reinterpret_cast<char *>(&header_), sizeof(compound_document_header));
}

void compound_document::write_msat()
{
    auto msat_sector = header_.extra_msat_start;

    for (auto i = std::uint32_t(0); i < header_.num_msat_sectors; ++i)
    {
        if (i < std::uint32_t(109))
        {
            header_.msat[i] = msat_[i];
        }
        else
        {
            auto sector = std::vector<sector_id>();
            auto sector_writer = binary_writer<sector_id>(sector);

            read_sector(msat_sector, sector_writer);

            msat_sector = sector.back();
            sector.pop_back();

            std::copy(sector.begin(), sector.end(), std::back_inserter(msat_));
        }
    }
}

void compound_document::write_sat()
{
    auto sector_reader = binary_reader<sector_id>(sat_);

    for (auto sat_sector : msat_)
    {
        write_sector(sector_reader, sat_sector);
    }
}

void compound_document::write_ssat()
{
    auto sector_reader = binary_reader<sector_id>(ssat_);

    for (auto ssat_sector : follow_chain(header_.ssat_start, sat_))
    {
        write_sector(sector_reader, ssat_sector);
    }
}

void compound_document::write_entry(directory_id id)
{
    const auto directory_chain = follow_chain(header_.directory_start, sat_);
    const auto entries_per_sector = sector_size() / sizeof(compound_document_entry);
    const auto directory_sector = directory_chain[id / entries_per_sector];
    const auto offset = sector_size() * directory_sector
        + ((id % entries_per_sector) * sizeof(compound_document_entry));

    out_->seekp(offset, std::ios::beg);
    out_->write(reinterpret_cast<char *>(&entries_[id]), sizeof(compound_document_entry));
}

} // namespace detail
} // namespace xlnt
