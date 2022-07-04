/***************************************************************************\
    server.cpp - iqdb server (database maintenance and queries)

    Copyright (C) 2008 piespy@gmail.com

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

#include <csignal>
#include <cstddef>
#include <cstring>
#include <string>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <algorithm>
#include <regex>

#include <iqdb/debug.h>
#include <iqdb/imgdb.h>
#include <iqdb/imglib.h>
#include <iqdb/haar_signature.h>
#include <iqdb/types.h>
#include <iqdb/MD5.h>

#include <httplib.h>
#include <nlohmann/json.hpp>

using nlohmann::json;
using httplib::Server;
using iqdb::IQDB;

namespace iqdb {

static Server server;

static void signal_handler(int signal, siginfo_t* info, void* ucontext) {
  INFO("Received signal {} ({})\n", signal, strsignal(signal));

  if (signal == SIGSEGV) {
    INFO("Address: {}\n", info->si_addr);
    DEBUG("{}", get_backtrace(2));
    exit(1);
  }

  if (server.is_running()) {
    server.stop();
  }
}

void install_signal_handlers() {
  struct sigaction action = {};
  sigfillset(&action.sa_mask);
  action.sa_flags = SA_RESTART | SA_SIGINFO;

  action.sa_sigaction = signal_handler;

  sigaction(SIGINT, &action, NULL);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGSEGV, &action, NULL);
}

void http_server(const std::string host, const int port, const std::string database_filename) {
  INFO("Starting server...\n");
  
  std::shared_mutex mutex_;
  auto memory_db = std::make_unique<IQDB>(database_filename);
  
  install_signal_handlers();
  
  // Adding Image
  // requires id, add or replace img if id exists
  server.Post("/images/(\\d+)", [&](const auto &request, auto &response) {
    std::unique_lock lock(mutex_);
    
    const postId post_id = std::stoi(request.matches[1]);
    const auto &file = request.get_file_value("file");
    std::string md5 = "";
    bool invalid_id = false;
    bool no_file = false;
    json data;
    
    // checking
    if (!request.has_file("file"))
      no_file = true;
    
    if (post_id <= 0)
      invalid_id = true;
    
    if (!invalid_id && !no_file)
    {
      // handle MD5 param
      if (request.has_param("md5")) {
        md5 = request.get_param_value("md5");
      } else {
        md5 = getMD5(file.content);
      }
      
      // add image & create response data
      try {
        const auto signature = HaarSignature::from_file_content(file.content);
        memory_db->addImage(post_id, md5, signature); // replace_img = true
        data = {
          { "post_id", post_id },
          { "md5", md5 },
          { "hash", signature.to_string() },
          { "signature", {
            { "avglf", signature.avglf },
            { "sig", signature.sig },
          }}
        };
      } catch (const image_error& e) { // catch image_error throw by IQDB::addImage()
        data = {
          { "error", e.what() },
          { "post_id", post_id },
          { "md5", md5 }
        };
        response.status = 400;
        DEBUG("Adding Error. post_id: {}, md5: {}, error: {}\n", post_id, md5, e.what());
      }
    }
    else if (invalid_id)
    {
      data = {
        { "error", "Input post_id must greater than 0." }
      };
      response.status = 400;
      DEBUG("Adding Error. Input post_id must greater than 0.");
    }
    else if (no_file)
    {
      data = {
        { "error", "`POST /images/:id?md5=M` requires a `file` param." }
      };
      response.status = 400;
      DEBUG("Adding Error. `POST /images/:id?md5=M` requires a `file` param.");
    }
    
    response.set_content(data.dump(4), "application/json");
  });
  
  // add new img with last post id
  server.Post("/images", [&](const auto &request, auto &response) {
    std::unique_lock lock(mutex_);
    
    const postId post_id = memory_db->getLastPostId()+1;
    const auto &file = request.get_file_value("file");
    std::string md5 = "";
    bool no_file = false;
    
    // checking
    if (!request.has_file("file"))
      no_file = true;
    
    if (!no_file)
    {
      // handle MD5 param
      if (request.has_param("md5")) {
        md5 = request.get_param_value("md5");
      } else {
        md5 = getMD5(file.content);
      }
      
      // add image & create response data
      json data;
      try {
        const auto signature = HaarSignature::from_file_content(file.content);
        memory_db->addImage(post_id, md5, signature, false); // replace_img = false
        data = {
          { "post_id", post_id },
          { "md5", md5 },
          { "hash", signature.to_string() },
          { "signature", {
            { "avglf", signature.avglf },
            { "sig", signature.sig },
          }}
        };
      } catch (const image_error& e) { // catch image_error throw by IQDB::addImage()
        data = {
          { "error", e.what() },
          { "post_id", post_id },
          { "md5", md5 }
        };
        response.status = 400;
        DEBUG("Adding Error. post_id: {}, md5: {}, error: {}\n", post_id, md5, e.what());
      }
    }
    else if (no_file)
    {
      data = {
        { "error", "`POST /images/:id?md5=M` requires a `file` param." }
      };
      response.status = 400;
      DEBUG("Adding Error. `POST /images/:id?md5=M` requires a `file` param.");
    }
    
    response.set_content(data.dump(4), "application/json");
  });
  
  // Removing images
  server.Delete("/images/([0-9a-fA-F]{0,32})", [&](const auto &request, auto &response) {
    std::unique_lock lock(mutex_);
    
    postId post_id = 0;
    std::string md5 = "";
    std::string tmp_param = request.matches[1];
    bool invalid_param = false;
    // tmp_param is post_id
    if (tmp_param.size() > 0 && tmp_param.size() <= 9 && std::all_of(tmp_param.begin(), tmp_param.end(), ::isdigit))
    {
      post_id = std::stoi(tmp_param);
      auto img = memory_db->getImage(post_id);
      if (img)
        md5 = img->md5;
    }
    // tmp_param is md5
    else if (tmp_param.size() == 32 && std::all_of(tmp_param.begin(), tmp_param.end(), ::isxdigit))
    {
      md5 = tmp_param;
      auto img = memory_db->getImageByMD5(md5);
      if (img)
        post_id = img->post_id;
    }
    // invalid tmp_param
    else
    {
      invalid_param = true;
    }
    
    bool ret = memory_db->removeImage(post_id);
    
    json data = {{}};
    if (ret) {
      data = {
        { "post_id", post_id },
        { "md5", md5 }
      };
    } else if (!invalid_param) { // valid param & ret false
      std::string msg = "Image does not exist in database.";
      if (post_id > 0)
        msg = "(" + std::regex_replace("post_id: {}", std::regex("\\{\\}"), std::to_string(post_id)) + ") " + msg;
      if (md5.size() == 32)
        msg = "(" + std::regex_replace("md5: {}", std::regex("\\{\\}"), md5) + ") " + msg;
      data = {
        { "error", msg }
      };
      response.status = 400;
      DEBUG("Removing Error. {}\n", msg);
    } else { // invalid param & ret false
      data = {
        { "error", "Invalid request url, you should supply integer post_id or md5 hash string (32-digit)." }
      };
      response.status = 400;
      DEBUG("Removing Error. Invalid request url, you should supply integer post_id or md5 hash string (32-digit).\n");
    }
    
    response.set_content(data.dump(4), "application/json");
  });
  
  // Searching for images
  server.Post("/query/([0-9a-fA-Fiqdb_file]+)", [&](const auto &request, auto &response) {
    std::shared_lock lock(mutex_);
    
    int limit = 10;
    sim_vector matches;
    json data = json::array();
    std::string tmp_param = request.matches[1];
    bool bad_request = false;
    bool couldnt_find_img = false;
    
    // handle param
    if (request.has_param("limit"))
      limit = stoi(request.get_param_value("limit"));
    
    // handle request url
    // input image file
    if (tmp_param == "file" && request.has_file("file"))
    {
      const auto &file = request.get_file_value("file");
      matches = memory_db->queryFromBlob(file.content, limit);
    }
    // input image haar hash
    else if (tmp_param.size() == 533 && tmp_param.substr(0, 5) == "iqdb_" && std::all_of(tmp_param.begin()+6, tmp_param.end(), ::isxdigit))
    {
      const auto hash = tmp_param;
      HaarSignature haar = HaarSignature::from_hash(hash);
      matches = memory_db->queryFromSignature(haar, limit);
      if (matches.size() == 0)
        couldnt_find_img = true;
    }
    // input image md5 hash
    else if (tmp_param.size() == 32 && std::all_of(tmp_param.begin(), tmp_param.end(), ::isxdigit))
    {
      const auto md5 = tmp_param;
      const auto img = memory_db->getImageByMD5(md5);
      if (img != std::nullopt)
        matches = memory_db->queryFromSignature(img->haar(), limit);
      else
        couldnt_find_img = true;
    }
    // invalid request url
    else
    {
      bad_request = true;
    }
    
    if (!bad_request && !couldnt_find_img)
    {
      // rm duplicate in matches
      // insert non-duplicate elements into tmp
      sim_vector tmp;
      tmp.reserve(matches.size());
      for (auto m : matches)
      {
        if (std::find(tmp.begin(), tmp.end(), m) == tmp.end())
          tmp.emplace_back(m);
      }
      // overwrite matches with tmp
      matches.assign(tmp.begin(), tmp.end());
      
      for (const auto &match : matches) {
        if (limit == 0)
          break;
        
        auto image = memory_db->getImage(match.id);
        auto haar = image->haar();
        
        data += {
          { "post_id", match.id },
          { "md5", image->md5 },
          { "score", match.score },
          { "hash", haar.to_string() },
          { "signature", {
            { "avglf", haar.avglf },
            { "sig", haar.sig },
          }}
        };
        
        limit--;
      }
    }
    else if (bad_request)
    {
      data = {
        { "error", "Invalid request url, you should supply `file` with image file, md5 hash string (32-digit), or haar hash string (start with `iqdb_`, 533-digit)." }
      };
      response.status = 400;
      DEBUG("Querying Error. Invalid request url, you should supply `file` with image file, md5 hash string (32-digit), or haar hash string (start with `iqdb_`, 533-digit).\n");
    }
    else if (couldnt_find_img)
    {
      data = {
        { "error", "Couldn't find image from supplied hash." }
      };
      response.status = 400;
      DEBUG("Couldn't find image from supplied hash.\n");
    }
    
    response.set_content(data.dump(4), "application/json");
  });
  
  // DB status
  server.Get("/status", [&](const auto &request, auto &response) {
    std::shared_lock lock(mutex_);
    
    const size_t count = memory_db->getImgCount();
    const postId post_id = memory_db->getLastPostId();
    json data = {
      {"image_count", count},
      {"last_post_id", post_id}
    };
    
    response.set_content(data.dump(4), "application/json");
  });
  
  server.set_logger([](const auto &req, const auto &res) {
    INFO("{} \"{} {} {}\" {} {}\n", req.remote_addr, req.method, req.path, req.version, res.status, res.body.size());
  });
  
  server.set_exception_handler([](const auto& req, auto& res, std::exception &e) {
    const auto name = demangle_name(typeid(e).name());
    const auto message = e.what();
    
    json data = {
      { "exception", name },
      { "message", message },
      { "backtrace", last_exception_backtrace }
    };
    
    DEBUG("Exception: {} ({})\n{}\n", name, message, last_exception_backtrace);
    
    res.set_content(data.dump(4), "application/json");
    res.status = 500;
  });
  
  INFO("Listening on {}:{}.\n", host, port);
  server.listen(host.c_str(), port);
  INFO("Stopping server...\n");
}

void help() {
  printf(
    "Usage: iqdb COMMAND [ARGS...]\n"
    "  iqdb http [host] [port] [dbfile]  Run HTTP server on given host/port.\n"
    "  iqdb help                         Show this help.\n"
  );
  
  exit(0);
}

}
