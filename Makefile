main: main.o
	gcc -o main main.c -lreadline

main.o: main.c
	gcc -c main.c

clean:
	rm *.o main