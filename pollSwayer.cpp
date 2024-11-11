#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <vector>

using namespace std;

// handle the client connection and send the vote to the server
void handleClientConnection(const string& serverName, int portNum, const string& firstName, const string& lastName, const string& vote) {
    // create a socket
    int clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if(clientSocket < 0) {
        cerr << "Failed to create socket." << endl;
        return;
    }

    // server address setup
    sockaddr_in serverAddress;
    serverAddress.sin_family = AF_INET; // IPv4
    serverAddress.sin_port = htons(portNum);    // port number in network byte order
    if(inet_pton(AF_INET, serverName.c_str(), &(serverAddress.sin_addr)) <= 0) {    // server name from string to network address 
        cerr << "Invalid server address." << endl;
        close(clientSocket);
        return;
    }

    // connect to the server
    if(connect(clientSocket, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) < 0) {
        cerr << "Failed to connect to the server." << endl;
        close(clientSocket);
        exit(1);
    }

    // receive the server's request
    char buffer[256];
    int bytesRead = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
    if(bytesRead > 1) {
        buffer[bytesRead] = '\0';
        cout << "Server message: " << buffer << endl;
    }

    // send the name to the server
    string name = firstName + " " + lastName;
    send(clientSocket, name.c_str(), name.size(), 0);

    // receive the server's request
    bytesRead = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
    if(bytesRead > 1) {
        buffer[bytesRead] = '\0';
        cout << "Server message: " << buffer << endl;
    }

    // send the vote to the server
    send(clientSocket, vote.c_str(), vote.size(), 0);

    // receive the server's response
    bytesRead = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
    if(bytesRead > 1) {
        buffer[bytesRead] = '\0';
        cout << "Server message: " << buffer << endl;
    }

    close(clientSocket);
}

int main(int argc, char* argv[]) {
    if(argc != 4) { // check the number of arguments
        cerr << "Usage: " << argv[0] << " [serverName] [portNum] [inputFile.txt]" << endl;
        return 1;
    }

    string serverName = argv[1];
    int portNum = atoi(argv[2]);
    string inputFile = argv[3];

    ifstream input(inputFile);  // open the input file in read mode
    if(!input.is_open()) {      // check if the file is opened successfully
        cerr << "Failed to open input file." << endl;
        return 1;
    }

    string line;    // we will read the file line by line
    vector<thread> threads; // vector of threads to handle the clients

    while(getline(input, line)) {
        istringstream lineStream(line); // string(each line) as a stream of characters
        string firstName, lastName, vote;
        if(!(lineStream >> firstName >> lastName >> vote)) {         // extract the name and vote from the line
            cerr << "Invalid input format." << endl;// if the extraction fails the line is invalid
            continue;
        }

        threads.emplace_back(handleClientConnection, serverName, portNum, firstName, lastName, vote);
    }

    for(auto& t : threads) {
        t.join();
    }

    input.close();
    return 0;
}