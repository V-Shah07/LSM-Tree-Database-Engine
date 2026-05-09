#pragma once


#include <string>
#include <vector>
struct SkipListNode
{
    std::string key;
    std::string value;
    bool isTombstone = false;
    std::vector<SkipListNode*> forward;
};

class SkipList
{
    


    public:

    void Put(const std::string& key, const std::string& val);
    std::string Get(const std::string& key);
    void Remove(const std::string& key);

    private:
    
    int getRandomLevel();
    SkipListNode* head = nullptr;
    static const int maxLevels = 16;
    int highestLevel = 0;
    
};
