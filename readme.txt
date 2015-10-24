1. Use select function to "listen" different fd(s) to prevent any blocking.

2. Use file lock provided by fcntl to inform read_server that certain part of the items is under modifying. 

3. Use in-server array to record which id(s) are locked. We have to do this because write_server cannot detect the file locks created by itself.
