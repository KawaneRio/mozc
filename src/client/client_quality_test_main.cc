// Copyright 2010-2020, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <iostream>  // NOLINT
#include <map>
#include <numeric>  // accumulate
#include <string>
#include <vector>

#include "base/file_stream.h"
#include "base/flags.h"
#include "base/init_mozc.h"
#include "base/logging.h"
#include "base/multifile.h"
#include "base/port.h"
#include "base/util.h"
#include "client/client.h"
#include "evaluation/scorer.h"
#include "protocol/commands.pb.h"

// Test data automatically generated by gen_client_quality_test_data.py
// TestCase test_cases[] is defined.
#include "client/client_quality_test_data.inc"
#include "absl/flags/flag.h"

DEFINE_string(server_path, "", "specify server path");
DEFINE_string(log_path, "", "specify log output file path");
DEFINE_int32(max_case_for_source, 500,
             "specify max test case number for each test sources");

namespace mozc {
bool IsValidSourceSentence(const std::string& str) {
  // TODO(noriyukit) Treat alphabets by changing to Eisu-mode
  if (Util::ContainsScriptType(str, Util::ALPHABET)) {
    LOG(WARNING) << "contains ALPHABET: " << str;
    return false;
  }

  // Source should not contain kanji
  if (Util::ContainsScriptType(str, Util::KANJI)) {
    LOG(WARNING) << "contains KANJI: " << str;
    return false;
  }

  // Source should not contain katakana
  std::string tmp, tmp2;
  Util::StringReplace(str, "ー", "", true, &tmp);
  Util::StringReplace(tmp, "・", "", true, &tmp2);
  if (Util::ContainsScriptType(tmp2, Util::KATAKANA)) {
    LOG(WARNING) << "contain KATAKANA: " << str;
    return false;
  }
  return true;
}

bool GenerateKeySequenceFrom(const std::string& hiragana_sentence,
                             std::vector<commands::KeyEvent>* keys) {
  CHECK(keys);
  keys->clear();

  std::string tmp, input;
  Util::HiraganaToRomanji(hiragana_sentence, &tmp);
  Util::FullWidthToHalfWidth(tmp, &input);

  for (ConstChar32Iterator iter(input); !iter.Done(); iter.Next()) {
    const char32 ucs4 = iter.Get();

    // TODO(noriyukit) Improve key sequence generation; currently, a few ucs4
    // codes, like FF5E and 300E, cannot be handled.
    commands::KeyEvent key;
    if (ucs4 >= 0x20 && ucs4 <= 0x7F) {
      key.set_key_code(static_cast<int>(ucs4));
    } else if (ucs4 == 0x3001 || ucs4 == 0xFF64) {
      key.set_key_code(0x002C);  // Full-width comma -> Half-width comma
    } else if (ucs4 == 0x3002 || ucs4 == 0xFF0E || ucs4 == 0xFF61) {
      key.set_key_code(0x002E);  // Full-width period -> Half-width period
    } else if (ucs4 == 0x2212 || ucs4 == 0x2015) {
      key.set_key_code(0x002D);  // "−" -> "-"
    } else if (ucs4 == 0x300C || ucs4 == 0xff62) {
      key.set_key_code(0x005B);  // "「" -> "["
    } else if (ucs4 == 0x300D || ucs4 == 0xff63) {
      key.set_key_code(0x005D);  // "」" -> "]"
    } else if (ucs4 == 0x30FB || ucs4 == 0xFF65) {
      key.set_key_code(0x002F);  // "・" -> "/"  "･" -> "/"
    } else {
      LOG(WARNING) << "Unexpected character: " << std::hex << ucs4 << ": in "
                   << input << " (" << hiragana_sentence << ")";
      return false;
    }
    keys->push_back(key);
  }

  // Conversion key
  {
    commands::KeyEvent key;
    key.set_special_key(commands::KeyEvent::SPACE);
    keys->push_back(key);
  }
  return true;
}

bool GetPreedit(const commands::Output& output, std::string* str) {
  CHECK(str);

  if (!output.has_preedit()) {
    LOG(WARNING) << "No result";
    return false;
  }

  str->clear();
  for (size_t i = 0; i < output.preedit().segment_size(); ++i) {
    str->append(output.preedit().segment(i).value());
  }

  return true;
}

bool CalculateBLEU(client::Client* client, const std::string& hiragana_sentence,
                   const std::string& expected_result, double* score) {
  // Prepare key events
  std::vector<commands::KeyEvent> keys;
  if (!GenerateKeySequenceFrom(hiragana_sentence, &keys)) {
    LOG(WARNING) << "Failed to generated key events from: "
                 << hiragana_sentence;
    return false;
  }

  // Must send ON first
  commands::Output output;
  {
    commands::KeyEvent key;
    key.set_special_key(commands::KeyEvent::ON);
    client->SendKey(key, &output);
  }

  // Send keys
  for (size_t i = 0; i < keys.size(); ++i) {
    client->SendKey(keys[i], &output);
  }
  VLOG(2) << "Server response: " << output.Utf8DebugString();

  // Calculate score
  std::string expected_normalized;
  Scorer::NormalizeForEvaluate(expected_result, &expected_normalized);
  std::vector<std::string> goldens;
  goldens.push_back(expected_normalized);
  std::string preedit, preedit_normalized;
  if (!GetPreedit(output, &preedit) || preedit.empty()) {
    LOG(WARNING) << "Could not get output";
    return false;
  }
  Scorer::NormalizeForEvaluate(preedit, &preedit_normalized);

  *score = Scorer::BLEUScore(goldens, preedit_normalized);

  VLOG(1) << hiragana_sentence << std::endl
          << "   score: " << (*score) << std::endl
          << " preedit: " << preedit_normalized << std::endl
          << "expected: " << expected_normalized;

  // Revert session to prevent server from learning this conversion
  commands::SessionCommand command;
  command.set_type(commands::SessionCommand::REVERT);
  client->SendCommand(command, &output);

  return true;
}

double CalculateMean(const std::vector<double>& scores) {
  CHECK(!scores.empty());
  const double sum = std::accumulate(scores.begin(), scores.end(), 0.0);
  return sum / static_cast<double>(scores.size());
}
}  // namespace mozc

int main(int argc, char* argv[]) {
  mozc::InitMozc(argv[0], &argc, &argv);

  mozc::client::Client client;
  if (!mozc::GetFlag(FLAGS_server_path).empty()) {
    client.set_server_program(FLAGS_server_path);
  }

  CHECK(client.IsValidRunLevel()) << "IsValidRunLevel failed";
  CHECK(client.EnsureSession()) << "EnsureSession failed";
  CHECK(client.NoOperation()) << "Server is not respoinding";

  std::map<std::string, std::vector<double> > scores;  // Results to be averaged

  for (mozc::TestCase* test_case = mozc::test_cases;
       test_case->source != nullptr; ++test_case) {
    const std::string& source = test_case->source;
    const std::string& hiragana_sentence = test_case->hiragana_sentence;
    const std::string& expected_result = test_case->expected_result;

    if (scores.find(source) == scores.end()) {
      scores[source] = std::vector<double>();
    }
    if (scores[source].size() >= mozc::GetFlag(FLAGS_max_case_for_source)) {
      continue;
    }

    VLOG(1) << "Processing " << hiragana_sentence;
    if (!mozc::IsValidSourceSentence(hiragana_sentence)) {
      LOG(WARNING) << "Invalid test case: " << std::endl
                   << "    source: " << source << std::endl
                   << "  hiragana: " << hiragana_sentence << std::endl
                   << "  expected: " << expected_result;
      continue;
    }

    double score;
    if (!mozc::CalculateBLEU(&client, hiragana_sentence, expected_result,
                             &score)) {
      LOG(WARNING) << "Failed to calculate BLEU score: " << std::endl
                   << "    source: " << source << std::endl
                   << "  hiragana: " << hiragana_sentence << std::endl
                   << "  expected: " << expected_result;
      continue;
    }
    scores[source].push_back(score);
  }

  std::ostream* ofs = &std::cout;
  if (!mozc::GetFlag(FLAGS_log_path).empty()) {
    ofs = new mozc::OutputFileStream(FLAGS_log_path.c_str());
  }

  // Average the scores
  for (std::map<std::string, std::vector<double> >::iterator it =
           scores.begin();
       it != scores.end(); ++it) {
    const double mean = mozc::CalculateMean(it->second);
    (*ofs) << it->first << " : " << mean << std::endl;
  }
  if (ofs != &std::cout) {
    delete ofs;
  }

  return 0;
}
