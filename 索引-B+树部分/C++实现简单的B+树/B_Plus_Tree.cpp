#include "B_Plus_Tree.hpp"

int Node::lower_bound(int key) {
	int left = -1, right = get_size();
	while (left + 1 < right) {
		int mid = (left + right) / 2;
		if (keys[mid] >= key) {
			right = mid;
		} else {
			left = mid;
		}
	}
	return right;
}
int Node::upper_bound(int key) {
	int left = -1, right = get_size();
	while (left + 1 < right) {
		int mid = (left + right) / 2;
		if (keys[mid] > key) {
			right = mid;
		} else {
			left = mid;
		}
	}
	return right;
}
int Node::internal_lookup(int key) {
	return max(0, upper_bound(key) - 1);
}
bool Node::leaf_lookup(int key) {
	int pos = lower_bound(key);
	if (pos == get_size() || keys[pos] != key) return false;
	return true;
}

int Node::get_size() {
	assert(keys.size() == children.size());
	return keys.size();
}

int Node::get_min_size() {
	return (maxLen + 1) / 2;
}

int Node::get_max_size() {
	return maxLen;
}

int Node::find_child_index(Node *node) {
	for (int i = 0; i < children.size(); i++) {
		if (children[i] == node) return i;
	}
	assert(0);
	return -1;
}

void Node::insert_pair(int pos, int key, Node *node) {
	keys.insert(keys.begin() + pos, key);
	children.insert(children.begin() + pos, node);
	assert(keys.size() == children.size());
	return ;
}
void Node::erase_pair(int pos) {
	keys.erase(keys.begin() + pos);
	children.erase(children.begin() + pos);
	assert(keys.size() == children.size());
	return ;
}


Node* BPlusTree::find_leaf_node(int key) {
	Node * node = root;
	while (!node->isLeaf) {
		Node * new_node = node->children[node->internal_lookup(key)];
		node = new_node;
	}
	return node;
}

void BPlusTree::insert_entry(int key) {
	Node * leaf_node = find_leaf_node(key);
	if (!leaf_node->leaf_lookup(key)) {
		int pos = leaf_node->lower_bound(key);
		leaf_node->insert_pair(pos, key, nullptr);
		if (leaf_node->get_size() >= leaf_node->get_max_size()) {
			Node * new_node = split(leaf_node);
			insert_into_parent(leaf_node, new_node);
		}
		maintain_parent(leaf_node);
	}
	return ;
}
    
Node* BPlusTree::split(Node* old_node) {
	int old_node_size = old_node->get_size() / 2;
	int new_node_size = old_node->get_size() - old_node_size;
	
	Node * new_node = new Node(old_node->isLeaf);
	new_node->parent = old_node->parent;

	if (old_node->isLeaf) {
		new_node->prev = old_node;
		new_node->next = old_node->next;
		old_node->next = new_node;
		if (new_node->next) new_node->next->prev = new_node;
	}
	
	for (int i = old_node_size; i < old_node->get_size(); i++) {
		new_node->insert_pair(i - old_node_size, old_node->keys[i], old_node->children[i]);
	}
	
	for (int i = 0; i < new_node_size; i++) {
		old_node->erase_pair(old_node->get_size() - 1);
	}
	
	for (int i = 0; i < new_node_size; i++) {
		maintain_child(new_node, i);
	}
	
	return new_node;
}

void BPlusTree::insert_into_parent(Node* old_node, Node* new_node) {
	if (old_node == root) {
		Node * new_root = new Node(false);
		new_root->insert_pair(0, old_node->keys[0], old_node); 
		new_root->insert_pair(1, new_node->keys[0], new_node);
		
		old_node->parent = new_root;
		new_node->parent = new_root;
		
		root = new_root;
	} else {
		Node * old_parent = old_node->parent;
		int pos = old_parent->find_child_index(old_node);
		old_parent->insert_pair(pos + 1, new_node->keys[0], new_node);
		if (old_parent->get_size() >= old_parent->get_max_size()) {
			Node * new_parent = split(old_parent);
			insert_into_parent(old_parent, new_parent);
		}
	}
	return ;
}

bool BPlusTree::delete_entry(int key) {
	Node * leaf = find_leaf_node(key);
	if (!leaf->leaf_lookup(key)) {
		return false;
	}
	
	int pos = leaf->lower_bound(key);
	leaf->erase_pair(pos);
	maintain_parent(leaf);
	if (leaf->get_size() >= leaf->get_min_size()) {
		return false;
	} else {
		coalesce_or_redistribute(leaf);
		return true;
	} 
	return false;
}

bool BPlusTree::coalesce_or_redistribute(Node* node) {
	if (node == root) {
		return adjust_root(node);
	} 
	if (node->get_size() >= node->get_min_size()) {
		return false;
	}
	Node * parent = node->parent;
	int idx = parent->find_child_index(node);
	Node * neighbor_node = (idx == 0 ? parent->children[idx + 1] :  parent->children[idx - 1]);
	if (node->get_size() + neighbor_node->get_size() >= 2 * node->get_min_size()) {
		redistribute(neighbor_node, node, idx);
		return false; 
	}
	return coalesce(neighbor_node, node, parent, idx);
}

bool BPlusTree::adjust_root(Node* old_root) {
	if (!old_root->isLeaf && old_root->get_size() == 1) {
		Node * left_node = old_root->children[0];
		left_node->parent = nullptr;
		root = left_node;
		free(old_root);
		return true;
	}
	return false;
}

void BPlusTree::redistribute(Node* neighbor_node, Node* old_node, int pos) {
	if (pos != 0) {
		old_node->insert_pair(0, neighbor_node->keys.back(), neighbor_node->children.back());
		neighbor_node->erase_pair(neighbor_node->get_size() - 1);
		maintain_parent(old_node);
		maintain_child(old_node, 0);
	}else {
		old_node->insert_pair(old_node->get_size(), neighbor_node->keys[0], neighbor_node->children[0]);
		neighbor_node->erase_pair(0);
		maintain_parent(neighbor_node);
		maintain_child(old_node, old_node->get_size() - 1);
	}
	return ;
}

bool BPlusTree::coalesce(Node* neighbor_node, Node* old_node, Node* parent, int pos) {
	if (pos == 0) {
		swap(neighbor_node, old_node);
		pos++;
	}
	for (int i = 0; i < old_node->get_size(); i++) {
		neighbor_node->insert_pair(neighbor_node->get_size(), old_node->keys[i], old_node->children[i]);
	}
	for (int i = 0; i < neighbor_node->get_size(); i++) {
		maintain_child(neighbor_node, i);
	}
	for (int i = 0; i < old_node->get_size(); i++) {
		old_node->erase_pair(old_node->get_size() - 1);
	}
	if (old_node->isLeaf) {
		erase_leaf(old_node);
	}
	free(old_node);
	parent->erase_pair(pos);
	maintain_parent(neighbor_node);
	return coalesce_or_redistribute(parent);
	
}

void BPlusTree::erase_leaf(Node* leaf) {
	if (leaf->prev) {
		leaf->prev->next = leaf->next;
	}
	if (leaf->next) {
		leaf->next->prev = leaf->prev;
	}
	return ;
}

void BPlusTree::maintain_parent(Node* node) {
	while (node->parent) {
		Node * parent = node->parent;
		int idx = parent->find_child_index(node);
		if(node->keys.front() == parent->keys[idx]){
			break;
		}
		parent->keys[idx] = node->keys.front();
    	node = parent;
	}
	
}

void BPlusTree::maintain_child(Node* node, int idx) {
	if (!node->isLeaf) {
		node->children[idx]->parent = node;
	}
}

void BPlusTree::printLevelOrder() {
    queue<Node*> q;
    q.push(root);
    int level = 0;
    while (!q.empty()) {
        int size = q.size();
        cout << "Level " << level++ << ": ";
        for (int i = 0; i < size; ++i) {
            Node* node = q.front();
            q.pop();
            cout << "[";
            for (size_t j = 0; j < node->keys.size(); ++j) {
                cout << node->keys[j];
                if (j < node->keys.size() - 1) cout << ", ";
            }
            cout << "] ";
            if (!node->isLeaf) {
                for (auto child : node->children) {
                    q.push(child);
                }
            }
        }
        cout << endl;
    }
}


int BPlusTree::count(int key) {
	Node * left = find_leaf_node(key);
	int count = 0;
	while (left) {
		int l = left->lower_bound(key);
		int r = left->upper_bound(key);
		count += r - l;
		left = left->next;
	}
	return count;
}
