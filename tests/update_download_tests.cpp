#include "update_download.hpp"
#include <cstdlib>
#include <iostream>

void require(bool condition,const char*message){if(!condition){std::cerr<<message<<'\n';std::exit(1);}}
int main(){
 require(validAvatarUpdateContentRange(L"bytes 100-199/1000",100,1000),"valid range rejected");
 require(!validAvatarUpdateContentRange(L"bytes 0-199/1000",100,1000),"wrong start accepted");
 require(!validAvatarUpdateContentRange(L"bytes 100-199/999",100,1000),"wrong total accepted");
 require(!validAvatarUpdateContentRange(L"bytes 100-1000/1000",100,1000),"out-of-bounds end accepted");
 require(!validAvatarUpdateContentRange(L"bytes 100-x/1000",100,1000),"malformed range accepted");
 std::cout<<"update download validation tests passed\n";
}
