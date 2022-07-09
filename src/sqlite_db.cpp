#include <fstream>
#include <iostream>
#include <optional>
#include <vector>
#include <system_error>

#include <iqdb/debug.h>
#include <iqdb/imglib.h>
#include <iqdb/sqlite_db.h>
#include <iqdb/types.h>

namespace iqdb {

using namespace sqlite_orm;

HaarSignature Image::haar() const {
  lumin_t avglf = { avglf1, avglf2, avglf3 };
  return HaarSignature(avglf, *(signature_t*)sig.data());
}

void SqliteDB::eachImage(std::function<void (const Image&)> func) {
  for (auto& image : storage_.iterate<Image>()) {
    func(image);
  }
}

int SqliteDB::getImgCount()
{
  std::unique_lock lock(sql_mutex_);
  
  auto results = storage_.count(&Image::post_id);
  if (!results) {
    DEBUG("Couldn't count post_id in sqlite database.\n");
    return 0;
  }
  return results;
}

postId SqliteDB::getMaxPostId()
{
  std::unique_lock lock(sql_mutex_);
  
  auto results = storage_.max(&Image::post_id);
  if (!results) {
    DEBUG("Couldn't count post_id in sqlite database.\n");
    return 0;
  }
  return *results;
}

std::optional<Image> SqliteDB::getImage(postId post_id) {
  std::unique_lock lock(sql_mutex_);
  
  auto results = storage_.get_all<Image>(where(c(&Image::post_id) == post_id));
  if (results.size() == 1) {
    return results[0];
  } else {
    DEBUG("Couldn't find post #{} in sqlite database.\n", post_id);
    return std::nullopt;
  }
}

std::optional<Image> SqliteDB::getImageByMD5(const std::string& md5) {
  std::unique_lock lock(sql_mutex_);
  auto results = storage_.get_all<Image>(where(c(&Image::md5) == md5));
  
  if (results.size() == 1) {
    return results[0];
  } else {
    DEBUG("Couldn't find md5 {} in sqlite database.\n", md5);
    return std::nullopt;
  }
}

int SqliteDB::addImage(postId post_id, const std::string& md5, HaarSignature signature, bool replace_img) {
  int id = -1;
  auto sig_ptr = (const char*)signature.sig;
  std::vector<char> sig_blob(sig_ptr, sig_ptr + sizeof(signature.sig));
  Image image {
    0, post_id, md5, signature.avglf[0], signature.avglf[1], signature.avglf[2], sig_blob
  };
  
  storage_.transaction([&] {
    try {
      if (replace_img)
        removeImage(post_id);
      id = storage_.insert(image);
      return true; // commit
    } catch (const std::system_error& e) {
      // post_id unique constraint failed
      if (e.code().value() == 19 && std::string(e.what()).find("images.post_id") != std::string::npos)
        id = -1;
      // md5 unique constraint failed
      else if (e.code().value() == 19 && std::string(e.what()).find("images.md5") != std::string::npos)
        id = -2;
      else 
        DEBUG("Unhandled SQLite exception, error code: {}, error msg: {}\n", e.code().value(), e.what());
      return false; // rollback
    }
  });
  
  return id;
}

void SqliteDB::removeImage(postId post_id) {
  storage_.remove_all<Image>(where(c(&Image::post_id) == post_id));
}

}
