// Vertex Data Structure
struct Organization {
    int64_t id{};
    char name[256]{};
    char url[256]{};
};

struct Place {
    int64_t id{};
    char name[256]{};
    char url[256]{};
};

struct Tag {
    int64_t id{};
    char name[256]{};
    char url[256]{};
};

struct TagClass {
    int64_t id{};
    char name[256]{};
    char url[256]{};
};

struct Comment {
    int64_t id{};
    char browserUsed[80]{};
    char creationDate[29]{};
    char locationIP[80]{};
    char content[2000]{};
    int32_t length{};
};

struct Forum {
    int64_t id{};
    char title[256]{};
    char creationDate[29]{};
};

struct Person {
    int64_t id{};
    char firstName[80]{};
    char lastName[80]{};
    char gender[80]{};
    char birthday[10]{};
    char email[256]{};
    char speaks[80]{};
    char browserUsed[80]{};
    char locationIP[80]{};
    char creationDate[29]{};
};

struct Post {
    int64_t id{};
    char name[256]{};
    char url[256]{};
};

// Edge Data Structure
struct Forum_hasMemberOrModerator_Person {
    char creationDate[29]{};
};

struct Person_knows_Person {
    char creationDate[29]{};
};

struct Person_likes_Comment {
    char creationDate[29]{};
};

struct Person_likes_Post {
    char creationDate[29]{};
};

struct Person_workOrStudyAt_Organization {
    int32_t classYear{};
};
