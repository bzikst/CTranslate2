#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <set>
#include <vector>
#include <nlohmann/json.hpp>

#include "ctranslate2/filesystem.h"

namespace ctranslate2 {
  namespace models {

    // A virtual model.bin backed by immutable byte shards. Only one shard is
    // open at a time, with a fixed 64 KiB read buffer and no reconstructed file.
    class ModelShardBuffer : public std::streambuf {
    public:
      ModelShardBuffer(const std::string& directory, std::istream& index) {
        index.seekg(0, std::ios::end);
        const auto index_size = index.tellg();
        if (index_size < 0 || index_size > 65536)
          throw std::runtime_error("Invalid model shard index length");
        index.seekg(0);
        const auto manifest = nlohmann::json::parse(index);
        const auto& files = manifest.at("files");
        if (!files.is_array() || files.empty() || files.size() > 256)
          throw std::runtime_error("Invalid model shard list");
        const auto total = checked_size(manifest.at("size"));
        std::set<std::string> names;
        for (const auto& file : files) {
          const auto name = file.at("path").get<std::string>();
          const std::string prefix = "model.bin.part-";
          if (name.size() <= prefix.size() || name.size() > 128
              || name.compare(0, prefix.size(), prefix) != 0
              || name.find_first_not_of("0123456789", prefix.size()) != std::string::npos
              || !names.insert(name).second)
            throw std::runtime_error("Invalid or duplicate model shard name");
          const auto size = checked_size(file.at("size"));
          if (size == 0 || size > 1900000000 || size > total - _size)
            throw std::runtime_error("Invalid model shard size");
          const auto path = directory + "/" + name;
          auto stream = open_file_read(path, std::ios::binary, true);
          stream.seekg(0, std::ios::end);
          if (stream.tellg() != std::streampos(size))
            throw std::runtime_error("Model shard length mismatch: " + path);
          _parts.push_back({path, _size, size});
          _size += size;
        }
        if (_size != total)
          throw std::runtime_error("Model shard total length mismatch");
        setg(_buffer.data(), _buffer.data(), _buffer.data());
      }


    protected:
      int_type underflow() override {
        if (gptr() != egptr())
          return traits_type::to_int_type(*gptr());
        const auto position = current_position();
        if (position == _size)
          return traits_type::eof();
        auto it = std::upper_bound(_parts.begin(), _parts.end(), position,
                                  [](std::streamoff value, const Part& part) {
                                    return value < part.start;
                                  });
        --it;  // The first shard always starts at zero.
        const auto part_index = static_cast<size_t>(it - _parts.begin());
        if (part_index != _open_part) {
          _file = open_file_read(it->path, std::ios::binary, true);
          _open_part = part_index;
        }
        _file.clear();
        _file.seekg(position - it->start);
        const auto count = static_cast<std::streamsize>(
          std::min<std::streamoff>(_buffer.size(), it->size - (position - it->start)));
        _file.read(_buffer.data(), count);
        if (!_file || _file.gcount() != count)
          throw std::runtime_error("Unable to read model shard: " + it->path);
        _buffer_start = position;
        setg(_buffer.data(), _buffer.data(), _buffer.data() + count);
        return traits_type::to_int_type(*gptr());
      }


      pos_type seekoff(off_type offset, std::ios_base::seekdir direction,
                       std::ios_base::openmode mode) override {
        if (!(mode & std::ios::in) || (mode & std::ios::out))
          return pos_type(off_type(-1));
        std::streamoff base;
        if (direction == std::ios::beg)
          base = 0;
        else if (direction == std::ios::cur)
          base = current_position();
        else if (direction == std::ios::end)
          base = _size;
        else
          return pos_type(off_type(-1));
        // Compare before addition, including offset == streamoff::min().
        if (offset < -base || offset > _size - base)
          return pos_type(off_type(-1));
        const auto next = base + offset;
        if (next >= _buffer_start && next <= _buffer_start + (egptr() - eback()))
          setg(eback(), eback() + (next - _buffer_start), egptr());
        else {
          _buffer_start = next;
          setg(_buffer.data(), _buffer.data(), _buffer.data());
        }
        return pos_type(next);
      }


      pos_type seekpos(pos_type position, std::ios_base::openmode mode) override {
        return seekoff(static_cast<off_type>(position), std::ios::beg, mode);
      }


    private:
      struct Part {
        std::string path;
        std::streamoff start;
        std::streamoff size;
      };

      static std::streamoff checked_size(const nlohmann::json& value) {
        if (!value.is_number_unsigned()
            || value.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()))
          throw std::runtime_error("Invalid model shard integer size");
        return static_cast<std::streamoff>(value.get<uint64_t>());
      }


      std::streamoff current_position() const {
        return _buffer_start + (gptr() - eback());
      }

      std::vector<Part> _parts;
      std::array<char, 65536> _buffer;
      std::ifstream _file;
      size_t _open_part = std::numeric_limits<size_t>::max();
      std::streamoff _size = 0;
      std::streamoff _buffer_start = 0;
    };

    struct ModelShardStream : private ModelShardBuffer, public std::istream {
      ModelShardStream(const std::string& directory, std::istream& index)
        : ModelShardBuffer(directory, index)
        , std::istream(static_cast<ModelShardBuffer*>(this))
      {
      }
    };

  }
}
