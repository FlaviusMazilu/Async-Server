Nume: Mazilu Flavius-Romeo
Grupă: 324CA

# **Tema 3**

## Implementare
* Pentru fiecare conexiune se creeaza un ``struct connection`` care va retine informatiile utile pentru o conexiune
precum
    - stariile in care se afla(CLOSED, DATA_SENT etc)
    - informatii despre fisierul cerut(path, size, tip<static/dinamic>)
    - informatiile necesare pentru io asincron(contextul, eventfd, iocb)

Este folosit un ``epoll`` pentru multiplexarea io. In acest epoll au fost introduse evenimente pentru
* `listenfd` -> fd pentru socketul care asteapta conexiuni (Eveniment EPOLLIN)
* `conn->sockfd` -> fd pentru socketul care a acceptat conexiunea EPOLLIN-> pentru cand clientul trimite o cerere serverului, EPOLLOUT pentru cand serverul ii trimite clientului raspunsul
* `conn->eventfd` -> atunci cand bufferul alocat pentru citirea asincrona a datelor dintr-un fisier se umple/se termina toate datele de citit, se scrie pe eventfd si semnalat in epoll, pentru a apela functia de scriere pe socketul client

### Flow-ul programului este urmatorul: 
- se asteapta conexiuni
- se accepta conexiunea 
- se asteapta un request 
- request-ul clientului este realizat in `handle_client_request()` unde se parseaza path-ul resursei dorite, se notifica epollout pe socketul clientului
- se incepe scrierea, prin functia send_message
    - daca fisirul este unul static, la inceputul functiei send_message se apeleaza `send_message_sendfile()`, altfel se continua cu executia(se ia asincron in memorie fisierul dorit(cel putin cat permite bufferul)) si se iese din aceasta functie
    - urmeaza sa fie notificat prin eventfd cand datele ajung in memorie, care eventfd semnalat in epoll apeleaza functia de scriere pe socket: `send_message_event_sig()`- 
    ea testeaza si daca a primit numarul maxim de bytes (DA -> mai programeaza odata aio read, altfel, incheie conexiunea)


Notes:
- Intreg enuntul temei a fost implementat, fara functionalitati extra. Dificultati in implementare: testare si depanare greoaie pana sa imi dau seama cum sa folosesc fisierul result.txt, chestia aia e geniala
- Partea de io asincron a fost bataie de cap sa imi dau seama cum sa semnalez si ce sa apelez. 
- Lucruri interesante descoperite pe parcurs: niciodata sa nu faci read fara while:)
- Tema mi s-a parut cea mai utila de pana acum, am invatat as zice multe lucruri, mai ales ce sa nu fac dar si cum sa fac research si mai ales depanat.
- Implementarea as spune ca e una mediocra, nici naiva nici eficienta, clar se putea mai bine facut

## Cum se compilează și cum se rulează?
* ``make build`` pentru a compila. Acesta construieste obiectele aws.o http_parser.o si sock_util.o, pe care le 
linkeaza cu biblioteca libaio in executabilul final ``aws``
* ``./aws`` pentru a porni serverul, din alt terminal ``WGET http://localhost:8888/<path>`` pentru a testa functionalitatea sau ``cp aws checker/ && make -f Makefile.checker`` pentru a rula suita de teste

## Bibliografie
* The Linux Programming Interface:
    https://sciencesoftcode.files.wordpress.com/2018/12/the-linux-programming-interface-michael-kerrisk-1.pdf
* https://eklitzke.org/blocking-io-nonblocking-io-and-epoll
* https://github.com/eklitzke/epollet/blob/master/poll.c
* https://ocw.cs.pub.ro/courses/so/laboratoare/laborator-11

