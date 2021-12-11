build: assignment2.c
	mpicc -w -o assignment2 assignment2.c -lm

run:
	mpirun -oversubscribe -np 13 assignment2 3 4

clean:
	/bin/rm -f assignment2 *.o
	/bin/rm -f base_station_event_log.txt
	/bin/rm -f base_station_fault_log.txt
	/bin/rm -f base_station_log.txt
	/bin/rm -f sensor_event_log.txt
