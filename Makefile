# compiler
CC = g++

# Compile options. Το -I<dir> λέει στον compiler να αναζητήσει εκεί include files
CFLAGS = -std=c++11
LFLAGS = -lpthread

# Τα εκτελέσιμα
SERV = poller
CLI = pollSwayer

# Παράμετροι για δοκιμαστική εκτέλεση linux01.di.uoa.gr
SERARGS = 5005 10 5 log.txt stat.txt
CLIARGS = 127.0.0.1 5005 inputFile

# Μεταγλώττιση poller
poller: $(SERV).cpp
	$(CC) $(CFLAGS) -o $(SERV) $(SERV).cpp $(LFLAGS)

# Μεταγλώττιση pollSwayer
pollSwayer: $(CLI).cpp
	$(CC) $(CLI).cpp -o $(CLI) $(CFLAGS) $(LFLAGS)

# Καθαρισμός
clean:
	rm -f $(SERV) $(CLI) log.txt stat.txt

# Εκτέλεση poller (καλυτερα να γινει απο το command line)
run_poller: $(SERV)
	./$(SERV) $(SERARGS)

# Εκτέλεση pollSwayer (καλυτερα να γινει απο το command line)
run_pollSwayer: $(CLI)
	./$(CLI) $(CLIARGS)