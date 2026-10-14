#pragma once
#include <stdint.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
// Sustituto de NVS SOLO para tests: conserva bytes entre instancias y simula
// fallos de escritura. El firmware real incluye Preferences del core ESP32.
struct FakeNvs {
  inline static std::map<std::string,std::vector<uint8_t>> data;
  inline static std::vector<std::pair<std::string,size_t>> writes;
  inline static int failAt=-1;
  static void reset(){data.clear();writes.clear();failAt=-1;}
};
struct FakeSerial {void println(const char*){}};
inline FakeSerial Serial;
class Preferences {
  std::string ns;
  std::string key(const char* k){return ns+"/"+k;}
public:
  bool begin(const char* name,bool=false){ns=name;return true;}
  void end(){}
  bool isKey(const char* k){return FakeNvs::data.count(key(k));}
  size_t getBytesLength(const char* k){auto it=FakeNvs::data.find(key(k));return it==FakeNvs::data.end()?0:it->second.size();}
  size_t getBytes(const char* k,void* out,size_t size){auto it=FakeNvs::data.find(key(k));if(it==FakeNvs::data.end()||it->second.size()>size)return 0;memcpy(out,it->second.data(),it->second.size());return it->second.size();}
  size_t putBytes(const char* k,const void* bytes,size_t size){FakeNvs::writes.push_back({key(k),size});if(int(FakeNvs::writes.size())==FakeNvs::failAt)return 0;auto b=static_cast<const uint8_t*>(bytes);FakeNvs::data[key(k)]={b,b+size};return size;}
};
