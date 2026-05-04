#include <random>

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

std::vector<double> master_weights = {0.0025, 0.0005, 0.0051, 0.0, 0.645, 0.0284, 0.0031, 0.3154};
std::discrete_distribution<> master_distribution(master_weights.begin(), master_weights.end());

std::vector<double> mirror_weights = {0.00417, 0.00353, 0.0681, 0.00022, 0.33865, 0.0, 0.05803, 0.5273};
std::discrete_distribution<> mirror_distribution(mirror_weights.begin(), mirror_weights.end());

std::vector<double> edge_weights = {
    0.0005, 0.0001, 0.0009, 0.0, 0.1189,
    0.1564, 0.1189, 0.0603, 0.0586, 0.0582,
    0.0987, 0.0180, 0.0133, 0.0006, 0.0105,
    0.0834, 0.0436, 0.0017, 0.0582, 0.0413,
    0.0582
};
std::discrete_distribution<> edge_distribution(edge_weights.begin(), edge_weights.end());

std::unique_ptr<Organization[]> organization_memory;
std::unique_ptr<Place[]> place_memory;
std::unique_ptr<Tag[]> tag_memory;
std::unique_ptr<TagClass[]> tagclass_memory;
std::unique_ptr<Comment[]> comment_memory;
std::unique_ptr<Forum[]> forum_memory;
std::unique_ptr<Person[]> person_memory;
std::unique_ptr<Post[]> post_memory;

std::unique_ptr<Forum_hasMemberOrModerator_Person[]> forum_person_memory;
std::unique_ptr<Person_knows_Person[]> person_person_memory;
std::unique_ptr<Person_likes_Comment[]> person_comment_memory;
std::unique_ptr<Person_likes_Post[]> person_post_memory;
std::unique_ptr<Person_workOrStudyAt_Organization[]> person_organization_memory;

// Setup Seeding Information
thread_local std::mt19937 generator(0);

std::vector<double> person_person_weights = {0.068434, 0.931566};
//std::vector<double> person_person_weights = {1, 0};
thread_local std::discrete_distribution<> person_person_distribution(person_person_weights.begin(), person_person_weights.end());

std::vector<double> person_university_weights = {0.003012, 0.996988};
thread_local std::discrete_distribution<> person_university_distribution(person_university_weights.begin(), person_university_weights.end());

std::vector<double> same_university_weights = {0.041445, 0.958555};
thread_local std::discrete_distribution<> same_university_distribution(same_university_weights.begin(), same_university_weights.end());
