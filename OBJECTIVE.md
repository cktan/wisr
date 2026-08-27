# OBJECTIVE

- design pgrock 
	- a postgres frontend that depends on rocksdb for storage
	- for OLTP
	- simulate full MVCC
	- support the same transaction levels 
		- read-commited
		- repeatable-read
		- serialize

