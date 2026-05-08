#include "gram_db.h"
#include <boost/algorithm/string.hpp>
#include <cmath>
#include <fstream>
#include <cstdio>

namespace rime {

  const string kGrammarFormatV1 = "Rime::Grammar/1.0";
  const string kGrammarFormatV2 = "Rime::Grammar/2.0";
  const string kGrammarFormatPrefix = "Rime::Grammar/";

  bool GramDb::Load() {
    LOG(INFO) << "loading gram db: " << file_path();

    if (IsOpen()) Close();
    if (!OpenReadOnly()) {
      LOG(ERROR) << "error opening gram db '" << file_path() << "'.";
      return false;
    }

    char* format_ptr = Find<char>(0);
    if (!format_ptr) {
        LOG(ERROR) << "file is empty.";
        return false;
    }

    size_t safe_len = 0;
    while (safe_len < 32 && format_ptr[safe_len] != '\0') {
        safe_len++;
    }
    string format_str(format_ptr, safe_len);

    if (!boost::starts_with(format_str, kGrammarFormatPrefix)) {
        LOG(ERROR) << "invalid or missing metadata.";
        Close();
        return false;
    }

    if (boost::starts_with(string(format_str), kGrammarFormatV1)) {
      is_darts_ = true;
      metadata_v1_ = Find<grammar::MetadataV1>(0);
      char* array = metadata_v1_->double_array.get();
      if (!array) {
        LOG(ERROR) << "double array image not found.";
        Close();
        return false;
      }
      darts_trie_->set_array(array, metadata_v1_->double_array_size);
      LOG(INFO) << "loaded legacy darts trie of size " << metadata_v1_->double_array_size << ".";
      return true;

    } else if (boost::starts_with(string(format_str), kGrammarFormatV2)) {
      is_darts_ = false;
      metadata_v2_ = Find<grammar::MetadataV2>(0);
      char* array = metadata_v2_->trie_data.get();
      if (!array) {
        LOG(ERROR) << "trie image not found.";
        Close();
        return false;
      }
      try {
        marisa_trie_.map(array, metadata_v2_->trie_size);
      } catch (const marisa::Exception& e) {
        LOG(ERROR) << "failed to map marisa trie: " << e.what();
        Close();
        return false;
      }
      values_array_ = metadata_v2_->values_data.get();
      if (!values_array_) {
        LOG(ERROR) << "values array not found.";
        Close();
        return false;
      }
      LOG(INFO) << "loaded marisa trie of size " << metadata_v2_->trie_size << ".";
      return true;
    }

    LOG(ERROR) << "unsupported grammar format.";
    Close();
    return false;
  }

  bool GramDb::Save() {
    LOG(INFO) << "saving gram db: " << file_path();

    if (is_darts_) {
      LOG(WARNING) << "Legacy Darts format is read-only. Save operation aborted.";
      return false; 
    } 

    if (marisa_trie_.num_keys() == 0) {
      LOG(ERROR) << "the trie has not been constructed!";
      return false;
    }
    
    return ShrinkToFit();
  }

  bool GramDb::Build(const vector<pair<string, double>>& data) {
    is_darts_ = false;
    
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
        double safe_weight = (kv.second > 0) ? kv.second : 1e-10; 
        mapped_values[agent.key().id()] = (std::max)(0, int(log(safe_weight) * kValueScale));
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
    if (!ifs) {
      std::remove(tmp_file.c_str());
      return false;
    }
    size_t trie_size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    size_t values_size = mapped_values.size();
    size_t values_bytes = values_size * sizeof(int);
    size_t image_size = trie_size + values_bytes;
    const size_t kReservedSize = 1024;

    if (!Create(image_size + kReservedSize + sizeof(grammar::MetadataV2))) {
      LOG(ERROR) << "Error creating gram db file '" << file_path() << "'.";
      ifs.close();
      std::remove(tmp_file.c_str());
      return false;
    }

    metadata_v2_ = Allocate<grammar::MetadataV2>();
    if (!metadata_v2_) {
      ifs.close();
      std::remove(tmp_file.c_str());
      return false;
    }

    char* array = Allocate<char>(trie_size);
    if (!array) {
      ifs.close();
      std::remove(tmp_file.c_str());
      return false;
    }
    
    ifs.read(array, trie_size);
    ifs.close();
    
    if (std::remove(tmp_file.c_str()) != 0) {
      LOG(WARNING) << "Failed to clean up temporary file: " << tmp_file 
                   << " (It may be locked by the system/antivirus).";
    }

    metadata_v2_->trie_data = array;
    metadata_v2_->trie_size = trie_size;

    int* val_array = Allocate<int>(values_size);
    if (!val_array) return false; 
    std::memcpy(val_array, mapped_values.data(), values_bytes);

    metadata_v2_->values_data = val_array;
    metadata_v2_->values_size = values_size;
    std::strncpy(metadata_v2_->format, kGrammarFormatV2.c_str(), kGrammarFormatV2.length());

    marisa_trie_.map(array, trie_size);
    values_array_ = val_array;

    return true;
  }

  int GramDb::Lookup(const string& context, const string& word, Match results[kMaxResults]) {
    if (is_darts_) {
      size_t node_pos = 0;
      size_t key_pos = 0;
      darts_trie_->traverse(context.c_str(), node_pos, key_pos);
      if (key_pos == context.length()) {
        Darts::DoubleArray::result_pair_type darts_results[kMaxResults];
        int count = darts_trie_->commonPrefixSearch(word.c_str(), darts_results, kMaxResults, 0, node_pos);
        for (int i = 0; i < count; ++i) {
          results[i].value = darts_results[i].value;
          results[i].length = darts_results[i].length;
        }
        return count;
      }
      return 0;
    } 
    else {
      string query = context + word;
      marisa::Agent agent;
      agent.set_query(query.c_str(), query.length());

      int count = 0;
      while (marisa_trie_.common_prefix_search(agent) && count < kMaxResults) {
        size_t matched_len = agent.key().length();
        if (matched_len > context.length()) {
          results[count].value = values_array_[agent.key().id()];
          results[count].length = matched_len - context.length();
          count++;
        }
      }
      return count;
    }
  }

}  // namespace rime