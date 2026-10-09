Objective: make this a module with clear C-like interface, exposing only C++ vector, C++ string and C++ string-view.

1. the sq/ project should look like sm/
2. a include/sq.h file 
3. replace the elog with another mechanism similar to sm/ that propagate error back via 
   return value and thread local storage
4. replace PG data structure that is awkward (e.g. List) with vector
5. adopt naming like sm/ i.e. lower case with _ , add namespace, etc.

