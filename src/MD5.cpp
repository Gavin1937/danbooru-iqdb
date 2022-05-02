
#include <iqdb/MD5.h>
#include <openssl/md5.h>

namespace iqdb
{

std::string getMD5(const std::string& data)
{
  unsigned char digest[MD5_DIGEST_LENGTH];
  char output[33];

  MD5( (unsigned char*)data.c_str(), data.size(), (unsigned char*)&digest );
  for (int i = 0; i < 16; i++)
    sprintf( &output[i*2], "%02x", (unsigned int)digest[i] );

  return output;
}

}
