#include "sanitizer_common/sanitizer_stacktrace.h"
#include <map>
#include <set>

#define MAXPATH 1000

#define HEX_LOG
#define PRINT_BAD_CASTING
#define PRINT_BAD_CASTING_FILE

enum rbtree_node_color { RED, BLACK };

//Paul: a tree node
typedef struct rbtree_node_t {
  void* key;
  void* value;
  struct rbtree_node_t* left;
  struct rbtree_node_t* right;
  struct rbtree_node_t* parent;
  enum rbtree_node_color color;
} *rbtree_node;

//Paul: root node of the tree
typedef struct rbtree_t {
  rbtree_node root;
} *rbtree;

//Paul: an entry in the object map 
//note, each entry has a tree associated to it
typedef struct ObjTypeMapEntry {
  uptr* ObjAddr;
  uptr* RuleAddr;
  uint64_t TypeHashValue;
  uint32_t HeapArraySize;
  int Offset;
  rbtree HexTree;
} ObjTypeMapEntry;

//Paul: very result entry
typedef struct VerifyResultEntry {
  uint64_t SrcHValue;
  uint64_t DstHValue;
  char VerifyResult;
} VerifyResultEntry;

//Paul: create a tree, note we have a tree per object map entry.
rbtree rbtree_create();

void* rbtree_lookup(rbtree t, void* key);
void rbtree_insert(rbtree t, void* key, void* value);
int rbtree_delete(rbtree t, void* key);
void write_log(char *result, char *filename);
