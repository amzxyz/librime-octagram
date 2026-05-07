#ifndef RIME_GRAM_DB_H_
#define RIME_GRAM_DB_H_

#include <marisa.h>
#include <rime/resource.h>
#include <rime/dict/mapped_file.h>

namespace rime {
  namespace grammar {

    struct Metadata {
      static const int kFormatMaxLength = 32;
      char format[kFormatMaxLength];
      uint32_t db_checksum;
      uint32_t trie_size;
      uint32_t values_size;
      OffsetPtr<char> trie_data;
      OffsetPtr<int> values_data;
    };

  }  // namespace grammar

  class GramDb : public MappedFile {
  public:
    struct Match {
      int value;
      size_t length;
    };

    static constexpr int kMaxResults = 8;
    static constexpr double kValueScale = 10000;

    GramDb(const path& file_path) : MappedFile(file_path) {}

    bool Load();
    bool Save();
    bool Build(const vector<pair<string, double>>& data);
    int Lookup(const string& context, const string& word, Match results[kMaxResults]);

  private:
    marisa::Trie trie_;
    int* values_array_ = nullptr;
    grammar::Metadata* metadata_ = nullptr;
  };

}  // namespace rime

#endif  // RIME_GRAM_DB_H_
