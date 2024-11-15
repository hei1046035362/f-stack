//#include "nlohmann/json.hpp"
#include "cmd/WsConsumer.h"
#include <iostream>
int main(){
        nlohmann::json j;  
    j["name"] = "Alice";  
    j["hobbies"] = nlohmann::json::array({ "reading", "painting", "swimming" }); 
     
    std::string json_str = j.dump(4); // 4表示缩进空格数  
    std::cout << json_str << std::endl;  
    std::cout << j["hobbies"]["reading"] << std::endl;

  
    return 0; 
}
