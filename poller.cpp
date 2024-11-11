#include <iostream> // using cout, cerr, endl,...
#include <fstream>  // using ifstream, ofstream
#include <vector>   // using vector
#include <mutex>    // using mutex, lock_guard, unique_lock
#include <condition_variable>   // using condition_variable
#include <pthread.h>    // using pthread_t, pthread_create, pthread_join, pthread_cancel, pthread_mutex_t, pthread_cond_t, ..
#include <csignal>  // using signal, SIGINT
#include <atomic>   // using atomic
#include <queue>    // using queue
#include <unistd.h> // using close, shutdown
#include <sys/socket.h> // using socket, bind, listen, accept, recv, send
#include <arpa/inet.h>  // using sockaddr_in, inet_pton
#include <unordered_map>    // using unordered_map

using namespace std;

// global variables:
int serverSocket;       // server socket (needs to be global so acceptConnections thread has easy access)
int bufferSize;         // size of the connection buffer (needs to be global so worker threads have easy access)
string pollLogFile;     // name of the poll log file    -//-
string pollStatsFile;   // name of the poll stats file  -//-
queue<int> connectionBuffer;    // buffer to store client sockets
pthread_cond_t bufferFull;  // condition variable to indicate that the buffer is full (pthread so that no objects are created and the thread using it can be cancelled)
pthread_cond_t bufferEmpty; // condition variable to indicate that the buffer is empty  -//-
pthread_mutex_t bufferMutex;    // mutex to synchronize access to the buffer            -//-
mutex logMutex;         // mutex to synchronize access to the log file
mutex pollDataMutex;    // mutex to synchronize access to the poll data maps
mutex signalMutex;      // mutex to synchronize access to the termination signal
condition_variable signalCV;    // condition variable to notify the master thread of the termination signal
vector<pthread_t> workerThreads;// vector of worker threads

// atomic boolean to track whether a termination signal has been received
atomic<bool> terminateSignal(false);    // (if one thread writes to an atomic object while another thread reads from it, the behavior is well-defined)

// data structure to store poll data
unordered_map<string, string> pollData; // <Voter Name, Party Voted>
unordered_map<string, int> pollStats;   // <Party Name, Vote Count>

// handle the SIGINT signal
void handleSignal(int signal) {
    if(signal == SIGINT) {
        cout << " signal received." << endl;
        {
            lock_guard<mutex> lock(signalMutex);    // lock_guard locks the mutex and automatically unlocks it when it goes out of scope
            terminateSignal = true;
        }
        signalCV.notify_all();
    }
}

// check if the voter already voted
bool hasVoted(const string& pollLogFile, const string& name) {
    unique_lock<mutex> logLock(logMutex);   // lock the log mutex before reading from the log file
    ifstream logFile(pollLogFile);          // open the log file in read mode
    if(logFile.is_open()) {
        // read the log file line by line and search for the voter's name
        string line;
        while(getline(logFile, line)) {
            // if the line contains the voter's name find() won't return npos
            if(line.find(name) != string::npos) {
                logFile.close();
                logLock.unlock();
                return true;
            }
        }
        logFile.close();
    }
    logLock.unlock();
    return false;
}

// handle client connections
void handleConnection(int clientSocket) {
    string sendNameMsg = "SEND NAME PLEASE";
    send(clientSocket, sendNameMsg.c_str(), sendNameMsg.size(), 0);

    char nameBuffer[256];
    ssize_t bytesRead = recv(clientSocket, nameBuffer, sizeof(nameBuffer) - 1, 0);  // we get the voter's name
    if(bytesRead < 0) { // check if the client response was received successfully
        cerr << "Failed to receive client response." << endl;
        close(clientSocket);
        return;
    }

    nameBuffer[bytesRead] = '\0';   // add null character to the end of the received data

    // if the voter already voted the vote won't be recorded
    if(hasVoted(pollLogFile, nameBuffer)) {
        string alreadyVotedMsg = "ALREADY VOTED";
        send(clientSocket, alreadyVotedMsg.c_str(), alreadyVotedMsg.size(), 0);
        close(clientSocket);
        return;
    }

    string sendVoteMsg = "SEND VOTE PLEASE";
    send(clientSocket, sendVoteMsg.c_str(), sendVoteMsg.size(), 0);

    char voteBuffer[256];
    bytesRead = recv(clientSocket, voteBuffer, sizeof(voteBuffer) - 1, 0);  // we get the party voted
    if(bytesRead < 0) { // check if the client response was received successfully
        cerr << "Failed to receive client response." << endl;
        close(clientSocket);
        return;
    }

    voteBuffer[bytesRead] = '\0';   // add null character to the end of the received data

    unique_lock<mutex> logLock(logMutex);   // lock the log mutex to wite to the log file uninterrupted
    ofstream logFile(pollLogFile, ios::app);// open the log file in append mode (since we cleared it's contents at the beginning)
    if(logFile.is_open()) { // check if the log file was opened successfully, if it has write the voter's name and the party voted
        logFile << nameBuffer << " " << voteBuffer << endl;
        logFile.close();
    }
    logLock.unlock();

    unique_lock<mutex> pollDataLock(pollDataMutex); // lock the poll data mutex to update the poll data uninterrupted
    pollData[nameBuffer] = voteBuffer;  // add the voter's name and the party voted to the poll data map
    if(pollStats.find(voteBuffer) != pollStats.end())   // update the party votes count
        pollStats[voteBuffer]++;
    else
        pollStats[voteBuffer] = 1;
    
    pollDataLock.unlock();

    // send the vote recorded message to the client
    string voteRecordedMsg = "VOTE for Party ";
    voteRecordedMsg += voteBuffer;
    voteRecordedMsg += " RECORDED";
    send(clientSocket, voteRecordedMsg.c_str(), voteRecordedMsg.size(), 0);

    close(clientSocket);
}

// worker threads
void* workerThreadFunc(void* arg) {
    while(!terminateSignal) {
        pthread_mutex_lock(&bufferMutex); // only one worker thread can access the buffer at a time

        // if the buffer is empty wait until there is a connection in the buffer
        while(connectionBuffer.empty()) {
            pthread_cond_wait(&bufferEmpty, &bufferMutex);
        }

        int clientSocket = connectionBuffer.front();// get the client socket from the front of the buffer
        connectionBuffer.pop();                     // and remove it from the queue

        pthread_cond_signal(&bufferFull); // notify acceptConnections that there is emptied space in the buffer
        pthread_mutex_unlock(&bufferMutex);  // allow other worker threads to access the buffer

        // handle the client connection
        handleConnection(clientSocket);
    }

    return nullptr;
}

// accepts connections and places them in the buffer
void* acceptConnectionsThreadFunc(void* arg) {
    while(!terminateSignal) {
        int clientSocket = accept(serverSocket, NULL, NULL);    // accept a connection
        if(clientSocket < 0) {  // check if the connection was accepted successfully
            cerr << "Failed to accept connection." << endl;
            continue;
        }

        // if the connection was accepted successfully lock the buffer mutex before accessing the buffer to place the client socket in the buffer
        pthread_mutex_lock(&bufferMutex);

        // wait until there is space available in the connection buffer
        while(connectionBuffer.size() >= static_cast<size_t>(bufferSize)) {
            pthread_cond_wait(&bufferFull, &bufferMutex); // release the lock and wait until the buffer is not full
        }

        connectionBuffer.push(clientSocket);    // the client socket is then placed in the buffer
        pthread_cond_signal(&bufferEmpty);  // notify the worker threads that there is a connection in the buffer
        pthread_mutex_unlock(&bufferMutex); // finally release the lock
    }

    return nullptr;
}

// write poll results to the pollStatsFile
void writePollResults(const string& pollStatsFile) {
    unique_lock<mutex> pollDataLock(pollDataMutex);     // use the poll data mutex to update the poll stats file safely
    ofstream pollStatsFileStream(pollStatsFile);        // open the poll stats file in write mode
    if(pollStatsFileStream.is_open()) { // check if the poll stats file was opened successfully, if it has write the poll results
        int totalVotes = 0;
        for(const auto& entry : pollStats) {        // const auto&: const reference to the pollStats map entry that automatically figures out the type of the entry
            const string& partyName = entry.first;  // party name (key), const string&: reference to a constant string  
            int voteCount = entry.second;           // vote count (value)
            pollStatsFileStream << partyName << " " << voteCount << endl;
            totalVotes += voteCount;
        }
        pollStatsFileStream << "TOTAL " << totalVotes << endl;
        pollStatsFileStream.close();
    }
    pollDataLock.unlock();
}

// master thread
void* masterThreadFunc(void* arg) { // arg needs to be a void pointer in order to be passed to pthread_create
    // set up signal handler for SIGINT
    signal(SIGINT, handleSignal);

    // create workerThreadsNum worker threads
    int workerThreadsNum = *static_cast<int*>(arg); // static_cast: converts arg from void* to int* and dereferences it to get the value
    for(int i = 0; i < workerThreadsNum; ++i) {
        pthread_t workerThread;
        pthread_create(&workerThread, nullptr, workerThreadFunc, nullptr);
        workerThreads.push_back(workerThread);
    }

    // create the acceptConnections thread (accepts connections and places them in the buffer)
    pthread_t acceptConnectionsThread;
    pthread_create(&acceptConnectionsThread, nullptr, acceptConnectionsThreadFunc, nullptr);

    // now just wait for the termination signal
    unique_lock<mutex> terminationLock(signalMutex);
    signalCV.wait(terminationLock, [&]() { return terminateSignal == true; });  // wait until the termination signal is received

    // cancel the acceptConnections thread and any remaining worker threads 
    pthread_cancel(acceptConnectionsThread);
    for(auto& workerThread : workerThreads) {
        pthread_cancel(workerThread);
    }

    terminationLock.unlock();

    writePollResults(pollStatsFile);    // write poll results to the pollStatsFile before terminating
    return nullptr;
}

int main(int argc, char* argv[]) {
    if(argc != 6) { // check the number of the arguments
        cerr << "Usage: " << argv[0] << " <port> <numWorkerThread> <bufferSize> <pollLog.txt> <pollStats.txt>" << endl;
        return 1;
    }

    int port = atoi(argv[1]);
    int workerThreadsNum = atoi(argv[2]);
    bufferSize = atoi(argv[3]);
    pollLogFile = argv[4];
    pollStatsFile = argv[5];

    // create the server socket: AF_INET: address family IPv4, SOCK_STREAM: TCP, 0 (value for Internet Protocol(IP))
    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if(serverSocket < 0) {
        cerr << "Failed to create socket." << endl;
        return 1;
    }

    // server address setup
    sockaddr_in serverAddress;
    serverAddress.sin_family = AF_INET; // IPv4 address
    serverAddress.sin_addr.s_addr = INADDR_ANY; // the server can accept connections from any IP address
    serverAddress.sin_port = htons(port);   // convert the port number to network byte order

    // bind the socket to the specified address and port
    if(bind(serverSocket, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) < 0) {
        cerr << "Failed to bind socket." << endl;
        return 1;
    }

    if(listen(serverSocket, 128) < 0) {   // listen for connections on the socket, 128: max number of connections in the queue
        cerr << "Failed to listen on socket." << endl;
        return 1;
    }

    cout << "Server started." << endl;

    // clear the contents of log file if it exists from a previous run to avoid false 'already voted'
    ofstream logFile(pollLogFile);
    logFile.close();

    // create the master thread
    pthread_t masterThread;
    pthread_create(&masterThread, nullptr, masterThreadFunc, &workerThreadsNum);

    // wait for the master thread to complete
    pthread_join(masterThread, nullptr);

    shutdown(serverSocket, SHUT_RDWR);    // shutdown the server socket to prevent error messages/losing any data
    close(serverSocket);
    cout << "Server terminated." << endl;
    return 0;
}