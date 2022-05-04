#Dependence Introduction
```
The ZigBee gateway demo depends on Ember ZNet stack v5.7.4.0, EM3588 Long 
Range USB Stick and raspberry PI platform. You need to get EmberZNet stack 
v5.7.4.0 and Simplicity Studio IDE from Silicon Labs company, 
buy a ZM3588S-USB-LR from CEL company,and buy a raspberry PI board in market. 
The raspberry PI board need to install raspbian system released on 2017-08-16 
or later than 2017-08-16.
```



#Compile Introduction
```
1. Config Simplicity Studio to use EmberZNet v5.7.4.0 stack, 
   and use Simplicity Studio IDE to open ember/ember.isc file(choose ZNet 
   UNIX HOST if needed), and generate all project files.

2. Copy all files in /home/pi/EmberZNet/v5.7.4.0/app/builder/ember except 
   build directory and Makefile* file into ember directory(replace all file).

3. Copy Ember ZNet stack v5.7.4.0 source code into raspberry PI system 
   /home/pi/EmberZNet/v5.7.4.0 directory. If you want to save Ember ZNet stack 
   in different directory, you need to update the EMBER_STACK_DIR path defined 
   in Makefile.

4. Copy ember/Makefile.ezsp file to 
   /home/pi/EmberZNet/v5.7.4.0/app/builder/ember directory.
   
5. Go to /home/pi/EmberZNet/v5.7.4.0/app/builder/ember directory, execute 
   'make -f Makefile.ezsp' command to compile ember stack library. The compile 
   result library file libem3588stack5.7.4.a locate in 
   /home/pi/EmberZNet/v5.7.4.0/app/builder/ember/build/lib.

6. Copy the libem3588stack5.7.4.a into ember/build/lib directory.

7. Copy all files in /home/pi/EmberZNet/v5.7.4.0/app/builder/ember except 
   build directory and Makefile file into ember directory.

8. Execute 'make APP=zb_gatewayd PROD=raspberry_pi' command to compile ZigBee 
   gateway demo.
```
