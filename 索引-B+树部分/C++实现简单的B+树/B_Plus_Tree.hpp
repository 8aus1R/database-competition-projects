#ifndef _B_Plus_Tree_H_
#define _B_Plus_Tree_H_
#include <bits/stdc++.h>
using namespace std;
const int maxLen = 4;
struct Node {
    bool isLeaf;
    vector<int> keys;
    vector<Node*> children;
    Node* parent;
    Node* prev;
    Node* next;

    Node(bool leaf) : isLeaf(leaf), parent(nullptr), prev(nullptr), next(nullptr) {}
    int lower_bound(int key);
    int upper_bound(int key);
    int internal_lookup(int key);
    bool leaf_lookup(int key);
    void insert_pair(int pos, int key, Node *node);
	void erase_pair(int pos);
	int get_size();
	int get_min_size();
	int get_max_size();
	int find_child_index(Node *node);
};

struct BPlusTree {
    Node* root;
    BPlusTree() {
        root = new Node(true);
    }
	Node* find_leaf_node(int key);
    void insert_entry(int key);
    
    Node* split(Node* old_node);
    void insert_into_parent(Node* old_node, Node* new_node);
    
    bool delete_entry(int key);
    bool coalesce_or_redistribute(Node* node);
    bool adjust_root(Node* old_root);
    void redistribute(Node* neighbor_node, Node* old_node, int pos);
    bool coalesce(Node* neighbor_node, Node* old_node, Node* parent, int pos);
	void erase_leaf(Node* leaf);
	
    void printLevelOrder();
    void maintain_parent(Node* node);
    void maintain_child(Node* node, int idx);
    
    int count(int key);
    
};
#endif

