#include <iostream>
using namespace std;

class Person {
private:
    string name;
    int age;
public:
    // Constructor
    Person(string n, int a) {
        name = n;
        age = a;
    }

    // Method to display info
    void display() {
        cout << "Name: " << name << ", Age: " << age << endl;
    }
};

int main() {
    Person p("Alice", 30);
    p.display();
    return 0;
}
