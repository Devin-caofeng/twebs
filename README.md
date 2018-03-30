twebs uses the GET/POST method to serve static content(text, HTML, GIF, and JPG ....files) out of ./
and to serve dynamic content by running CGI programs out of ./cgi-bin.

twebs provides following functions:
- provide GET/POST method to view static content and run cgi script
- provide dircetory view function
- provide some access control based by ip address
- provide easy logging function

To run twebs:

    There are several argument option:
    -d  --daemon        -> twebs run in the background
    -p  --port          -> assigned http port
    -s  --sslport       -> assigned https port
    -l  --log           -> assigned log path
    -e  --extent        -> provide https function
    -h  --help          -> help
    -v  --version       -> some other informations

    So, you can also do this: ./twebs -d -p 8000 -s 6666 -e

    Point your browser at twebs: for example, http port is 8000, and https port is 6666
    you can view following page:
   	http:
        127.0.0.1:8000       home page

        127.0.0.1:8000/dir   view dir
            you can point the file name to view file contents or point the directory name to view the directory content

        127.0.0.1:8000/getAuth.html
            a GET method page,login with email and password, you will see you email and password content

        127.0.0.1:8000/postAuth.html
            a POST  method page,login with email and password, you will see you email and password content

    https: the same function as to http, just uri has a litter changes
        https://127.0.0.1:6666
        https://127.0.0.1:6666/dir
        https://127.0.0.1:6666/getAuth.html
        https://127.0.0.1:6666/postAuth.html


Files:

    cert.pem            ->  the https CA,use openssl to create, so you must accept the CA to continue
    cgi-bin             ->  cgi script directory
        getAuth.c       ->  the get method cgi script
        postAuth.c      ->  the post method cgi script
        Makefile        ->  cgi/bin/*.c Makefile
    config.ini          ->  configuration file
    daemon_init.c       ->  daemon process
    doc                 ->  the web page root directory
    log.c               ->  provide logging
    main.c              ->  the main source file
    Makefile            ->  *.c Makefile
    parse_config.c      ->  read the config.ini
    parse.h             ->  the main head file
    parse_option.c      ->  parse the argv
    README              ->  it's me
    secure_access.c     ->  provide easy access control
    webserver.sh        ->  a shell script, to provide start/stop/restart/status the twebs e.g. webserver.sh start/stop/restart/status
    wrap.c              ->  must functions wrap file
    wrap.h              ->  the wrap.c's head file
