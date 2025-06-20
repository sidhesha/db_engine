#include "schema.hpp"
#include <sstream>

Schema::Schema(const std::vector<Column>& columns): columns(columns) {};

const std::vector<Column>& Schema::getColumns()const{
    return columns;
}

std::string Schema::serialize() const{
    std::ostringstream oss;
    for(auto &col:columns){
        oss << col.name << ":" << col.type <<",";
    }
    std::string result = oss.str();
    if(!result.empty()) result.pop_back(); //removing last ',' for cleaner look
    return result;
}

Schema Schema::deserialize(const std::string &data){
    std::vector<Column> cols;
    std::istringstream iss(data);
    std::string token;
    while(getline(iss,token,',')){
        size_t pos = token.find(':');
        if(pos==std::string::npos) continue;
        std::string name = token.substr(0,pos);
        std::string type = token.substr(pos+1);
        cols.push_back({name,type});
    }
    return Schema(cols);
}