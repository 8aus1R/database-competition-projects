#include "B_Plus_Tree_Test.hpp"
bool test() {
    mt19937_64 rnd(random_device{}());
    int res = 0;
    vector<array<int, 2>> v; 
    BPlusTree tree;
    set<int> Set;
    for(int i = 0; i < 10000; i++) {
        int op = rnd() % 2, x = rnd() % 500;
        v.push_back({op, x});
        if(op == 0){
            if(Set.count(x) != tree.count(x)){
                res = 1;
                goto G;
            }
            tree.insert_entry(x);
            Set.insert(x);
            if(Set.count(x) != tree.count(x)){
                res = 1;
                goto G;
            }
        }
		else{
            if(Set.count(x) != tree.count(x)){
                res = 1;
                goto G;
            }
            tree.delete_entry(x);
            Set.erase(x);
            if(Set.count(x) != tree.count(x)){
                res = 1;
                goto G;
            }
        }
    }
    G:;
    if (res) {
        cout << "No" << endl;
        for(int i = 0; i < v.size(); i++){
            cout << v[i][0] << " " << v[i][1] << "\n";
        }
        return false;
    } else {
        cout << "Yes" << endl;
        return true;
    }
}
