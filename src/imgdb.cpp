/***************************************************************************\
    imgdb.cpp - iqdb library implementation

    Copyright (C) 2008 piespy@gmail.com

    Originally based on imgSeek code, these portions
    Copyright (C) 2003 Ricardo Niederberger Cabral.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
\**************************************************************************/

#include <sys/mman.h>

#include <algorithm>
#include <memory>
#include <vector>

#include <iqdb/debug.h>
#include <iqdb/imgdb.h>
#include <iqdb/imglib.h>
#include <iqdb/haar_signature.h>
#include <iqdb/sqlite_db.h>

namespace iqdb {

void bucket_set::add(const HaarSignature &sig, imageId iqdb_id) {
  eachBucket(sig, [&](auto& bucket) {
    bucket.push_back(iqdb_id);
  });
}

void bucket_set::remove(const HaarSignature &sig, imageId iqdb_id) {
  eachBucket(sig, [&](auto& bucket) {
    // https://en.wikipedia.org/wiki/Erase-remove_idiom
    bucket.erase(std::remove(bucket.begin(), bucket.end(), iqdb_id), bucket.end());
  });
}

bucket_t& bucket_set::at(int color, int coef) {
  const int sign = coef < 0;
  return buckets[color][sign][abs(coef)];
}

void bucket_set::eachBucket(const HaarSignature &sig, std::function<void(bucket_t&)> func) {
  for (int c = 0; c < sig.num_colors(); c++) {
    for (int i = 0; i < NUM_COEFS; i++) {
      const int coef = sig.sig[c][i];
      auto& bucket = at(c, coef);
      func(bucket);
    }
  }
}

void IQDB::addImage(imageId post_id, const std::string& md5, const HaarSignature& haar, bool replace_img) {
  
  if (replace_img)
    removeImage(post_id);
  
  int iqdb_id = sqlite_db_->addImage(post_id, md5, haar, replace_img);
  // fail to insert into sqlite db
  if (iqdb_id == -1) { // post_id unique constraint failed
    // current last_post_id may be out dated, update it
    last_post_id = sqlite_db_->getMaxPostId();
    
    DEBUG("post_id UNIQUE constrain failed. post_id={}, md5={}\n", post_id, md5);
    throw image_error("post_id UNIQUE constrain failed, this post_id already in database.");
  }
  else if (iqdb_id == -2) { // md5 unique constraint failed
    DEBUG("MD5 UNIQUE constrain failed. post_id={}, md5={}\n", post_id, md5);
    throw image_error("MD5 UNIQUE constrain failed, this MD5 already in database.");
  }
  addImageInMemory(iqdb_id, post_id, haar);
  
  last_post_id++;
  
  DEBUG("Added post #{} to memory and database (iqdb={} md5={} haar={}).\n", post_id, iqdb_id, md5, haar.to_string());
}

void IQDB::addImageInMemory(imageId iqdb_id, imageId post_id, const HaarSignature& haar) {
  if ((size_t)post_id >= m_info.size()) {
    DEBUG("Growing m_info array (size={}).\n", m_info.size());
    m_info.resize(post_id + 50000);
  }
  
  imgbuckets.add(haar, iqdb_id);
  
  image_info& info = m_info.at(iqdb_id);
  info.id = post_id;
  info.avgl.v[0] = static_cast<Score>(haar.avglf[0]);
  info.avgl.v[1] = static_cast<Score>(haar.avglf[1]);
  info.avgl.v[2] = static_cast<Score>(haar.avglf[2]);
}

void IQDB::loadDatabase(std::string filename) {
  sqlite_db_ = std::make_unique<SqliteDB>(filename);
  m_info.clear();
  imgbuckets = bucket_set();

  sqlite_db_->eachImage([&](const auto& image) {
    addImageInMemory(image.id, image.post_id, image.haar());

    if (image.id % 250000 == 0) {
      INFO("Loaded image {} (post #{})...\n", image.id, image.post_id);
    }
  });

  INFO("Loaded {} images from {}.\n", getImgCount(), filename);
}

bool IQDB::isDeleted(imageId iqdb_id) {
  return !m_info.at(iqdb_id).avgl.v[0];
}

std::optional<Image> IQDB::getImage(imageId post_id) {
  return sqlite_db_->getImage(post_id);
}

std::optional<Image> IQDB::getImageByMD5(const std::string& md5) {
  return sqlite_db_->getImageByMD5(md5);
}

sim_vector IQDB::queryFromBlob(const std::string blob, int numres) {
  HaarSignature signature = HaarSignature::from_file_content(blob);
  return queryFromSignature(signature, numres);
}

sim_vector IQDB::queryFromSignature(const HaarSignature &signature, size_t numres) {
  Score scale = 0;
  std::vector<Score> scores(m_info.size(), 0);
  std::priority_queue<sim_value> pqResults; /* results priority queue; largest at top */
  sim_vector V; /* output results */

  DEBUG("Querying signature={} json={}\n", signature.to_string(), signature.to_json());

  // Luminance score (DC coefficient).
  for (size_t i = 0; i < scores.size(); i++) {
    auto image_info = m_info[i];
    Score s = 0;

    for (int c = 0; c < signature.num_colors(); c++) {
      s += weights[0][c] * std::abs(image_info.avgl.v[c] - static_cast<Score>(signature.avglf[c]));
    }

    scores[i] = s;
  }

  for (int c = 0; c < signature.num_colors(); c++) {
    for (int b = 0; b < NUM_COEFS; b++) { // for every coef on a sig
      const int coef = signature.sig[c][b];
      auto &bucket = imgbuckets.at(c, coef);

      if (bucket.empty())
        continue;

      const int w = imgBin.bin[abs(coef)];
      Score weight = weights[w][c];
      scale -= weight;

      for (auto index : bucket) {
        scores[index] -= weight;
      }
    }
  }

  // Fill up the numres-bounded priority queue (largest at top):
  iqdbId i = 0;
  for (; pqResults.size() < numres && i < scores.size(); i++) {
    if (!isDeleted(i))
      pqResults.emplace(i, scores[i]);
  }

  for (; i < scores.size(); i++) {
    if (!isDeleted(i) && scores[i] < pqResults.top().score) {
      pqResults.pop();
      pqResults.emplace(i, scores[i]);
    }
  }

  if (scale != 0)
    scale = static_cast<Score>(1.0) / scale;

  while (!pqResults.empty()) {
    auto value = pqResults.top();
    value.id = m_info[value.id].id; // XXX replace iqdb id with post id
    value.score = value.score * 100 * scale;

    V.push_back(value);
    pqResults.pop();
  }

  std::reverse(V.begin(), V.end());
  return V;
}

bool IQDB::removeImage(imageId post_id) {
  auto image = sqlite_db_->getImage(post_id);
  if (image == std::nullopt) {
    WARN("Couldn't remove post #{}; post not in sqlite database.\n", post_id);
    return false;
  }
  
  imgbuckets.remove(image->haar(), image->id);
  m_info.at(image->id).avgl.v[0] = 0;
  sqlite_db_->removeImage(post_id);
  
  last_post_id--;
  
  DEBUG("Removed post #{} from memory and database.\n", post_id);
  return true;
}

bool IQDB::removeImageByMD5(const std::string& md5) {
  auto image = sqlite_db_->getImageByMD5(md5);
  if (image == std::nullopt) {
    WARN("Couldn't remove file with md5 {}; this md5 is not in sqlite database.\n", md5);
    return false;
  }
  
  imgbuckets.remove(image->haar(), image->id);
  m_info.at(image->id).avgl.v[0] = 0;
  sqlite_db_->removeImage(image->post_id);
  
  last_post_id--;
  
  DEBUG("Removed post #{} from memory and database.\n", image->post_id);
  return true;
}

size_t IQDB::getImgCount() {
  return sqlite_db_->getImgCount();
}

postId IQDB::getLastPostId() {
  return last_post_id;
}

IQDB::IQDB(std::string filename) : sqlite_db_(nullptr) {
  loadDatabase(filename);
  last_post_id = sqlite_db_->getMaxPostId();
}

}
