#include <iostream>
using namespace std;

class BankAccount {
private:
    string owner;
    double balance;
public:
    // Constructor
    BankAccount(string name, double initialBalance) {
        owner = name;
        balance = initialBalance;
    }

    // Deposit money
    void deposit(double amount) {
        if (amount > 0) {
            balance += amount;
            cout << "Deposited $" << amount << endl;
        } else {
            cout << "Invalid deposit amount." << endl;
        }
    }

    // Withdraw money
    void withdraw(double amount) {
        if (amount > 0 && amount <= balance) {
            balance -= amount;
            cout << "Withdrew $" << amount << endl;
        } else {
            cout << "Invalid or insufficient funds for withdrawal." << endl;
        }
    }

    // Show balance
    void showBalance() {
        cout << owner << "'s Balance: $" << balance << endl;
    }
};

int main() {
    BankAccount account("Bob", 1000.0);
    account.showBalance();
    account.deposit(500.0);
    account.showBalance();
    account.withdraw(200.0);
    account.showBalance();
    account.withdraw(2000.0); // Should show error
    return 0;
}
