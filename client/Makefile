server:server.cpp
	g++ -std=c++17 -DCPPHTTPLIB_OPENSSL_SUPPORT $^ -o $@ -I ../include -lssl -lcrypto -lboost_filesystem -lboost_system -lpthread -lz
perm:
	#使用openssl生成一个RSA密钥
	openssl genrsa 2048 > key.pem
	openssl req -new -key key.pem | openssl x509 -days 3650 -req -signkey key.pem > cert.pem
