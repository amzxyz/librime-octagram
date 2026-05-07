#include "gram_db.h"
#include <boost/algorithm/string.hpp>
#include <cmath>
#include <fstream>
#include <cstdio>
#include <rime/resource.h>
#include <rime/dict/mapped_file.h>

namespace rime {

  const string kGrammarFormat = "Rime::Grammar/2.0";
  const string kGrammarFormatPrefix = "Rime::Grammar/";

  bool GramDb::Load() {
    LOG(INFO) << "loading gram db: " << file_path();

    if (IsOpen()) Close();
    if (!OpenReadOnly()) {
      LOG(ERROR) << "error opening gram db '" << file_path() << "'.";
      return false;
    }

    metadata_ = Find<grammar::Metadata>(0);
    if (!metadata_ || !boost::starts_with(string(metadata_->format), kGrammarFormatPrefix)) {
      LOG(ERROR) << "invalid or missing metadata.";
      Close();
      return false;
    }

    char* array = metadata_->trie_data.get();
    if (!array) {
      LOG(ERROR) << "trie image not found.";
      Close();
      return false;
    }

    try {
      trie_.map(array, metadata_->trie_size);
    } catch (const marisa::Exception& e) {
      LOG(ERROR) << "failed to map marisa trie: " << e.what();
      Close();
      return false;
    }

    values_array_ = metadata_->values_data.get();
    if (!values_array_) {
      LOG(ERROR) << "values array not found.";
      Close();
      return false;
    }

    LOG(INFO) << "loaded marisa trie of size " << metadata_->trie_size << ".";
    return true;
  }

  bool GramDb::Save() {
    LOG(INFO) << "saving gram db: " << file_path();
    if (trie_.num_keys() == 0) {
      LOG(ERROR) << "the trie has not been constructed!";
      return false;
    }
    return ShrinkToFit();
  }

  bool GramDb::Build(const vector<pair<string, double>>& data) {
    marisa::Keyset keyset;
    for (const auto& kv : data) {
      keyset.push_back(kv.first.c_str(), kv.first.length(), 0.0);
    }

    marisa::Trie temp_trie;
    temp_trie.build(keyset);

    std::vector<int> mapped_values(temp_trie.num_keys(), 0);
    for (const auto& kv : data) {
      marisa::Agent agent;
      agent.set_query(kv.first.c_str(), kv.first.length());
      if (temp_trie.lookup(agent)) {
        mapped_values[agent.key().id()] = (std::max)(0, int(log(kv.second) * kValueScale));
      }
    }

    string tmp_file = file_path().string() + ".tmp";
    try {
      temp_trie.save(tmp_file.c_str());
    } catch (const marisa::Exception& e) {
      LOG(ERROR) << "Error saving temporary trie: " << e.what();
      return false;
    }

    std::ifstream ifs(tmp_file, std::ios::binary | std::ios::ate);
    if (!ifs) return false;
    size_t trie_size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    size_t values_size = mapped_values.size();
    size_t values_bytes = values_size * sizeof(int);
    size_t image_size = trie_size + values_bytes;
    const size_t kReservedSize = 1024;

    if (!Create(image_size + kReservedSize + sizeof(grammar::Metadata))) {
      LOG(ERROR) << "Error creating gram db file '" << file_path() << "'.";
      return false;
    }

    metadata_ = Allocate<grammar::Metadata>();
    if (!metadata_) return false;

    char* array = Allocate<char>(trie_size);
    if (!array) return false;
    ifs.read(array, trie_size);
    ifs.close();
    std::remove(tmp_file.c_str());

    metadata_->trie_data = array;
    metadata_->trie_size = trie_size;

    int* val_array = Allocate<int>(values_size);
    if (!val_array) return false;
    std::memcpy(val_array, mapped_values.data(), values_bytes);

    metadata_->values_data = val_array;
    metadata_->values_size = values_size;
    std::strncpy(metadata_->format, kGrammarFormat.c_str(), kGrammarFormat.length());

    trie_.map(array, trie_size);
    values_array_ = val_array;

    return true;
  }

  int GramDb::Lookup(const string& context, const string& word, Match results[kMaxResults]) {
    string query = context + word;
    marisa::Agent agent;
    agent.set_query(query.c_str(), query.length());

    int count = 0;
    while (trie_.common_prefix_search(agent) && count < kMaxResults) {
      size_t matched_len = agent.key().length();
      if (matched_len > context.length()) {
        results[count].value = values_array_[agent.key().id()];
        results[count].length = matched_len - context.length();
        count++;
      }
    }
    return count;
  }

}  // namespace rime
