# vim:set ft= ts=4 sw=4 et fdm=marker:

use lib 'lib';
use Test::Nginx::Socket;

#worker_connections(1014);
#master_on();
#workers(2);
#log_level('warn');

repeat_each(2);

plan tests => repeat_each() * (blocks() * 4);

our $StapOutputChains = <<'_EOC_';
global active

F(ngx_http_handler) {
    active = 1
}

/*
F(ngx_http_write_filter) {
    if (active && pid() == target()) {
        printf("http writer filter: %s\n", ngx_chain_dump($in))
    }
}
*/

F(ngx_http_chunked_body_filter) {
    if (active && pid() == target()) {
        printf("http chunked filter: %s\n", ngx_chain_dump($in))
    }
}

probe syscall.writev {
    if (active && pid() == target()) {
        printf("writev(%s)", ngx_iovec_dump($vec, $vlen))
        /*
        for (i = 0; i < $vlen; i++) {
            printf(" %p [%s]", $vec[i]->iov_base, text_str(user_string_n($vec[i]->iov_base, $vec[i]->iov_len)))
        }
        */
    }
}

probe syscall.writev.return {
    if (active && pid() == target()) {
        printf(" = %s\n", retstr)
    }
}

_EOC_

#no_diff();
#no_long_string();
run_tests();

__DATA__

=== TEST 1: ambiguous pattern
--- config
    default_type text/html;
    location /t {
        echo abcabcabde;
        replace_filter abcabd X;
    }
--- request
GET /t
--- response_body
abcXe
--- no_error_log
[alert]
[error]



=== TEST 2: ambiguous pattern
--- config
    default_type text/html;
    location /t {
        echo -n ababac;
        replace_filter abac X;
    }
--- request
GET /t
--- response_body chop
abX
--- no_error_log
[alert]
[error]



=== TEST 3: alt
--- config
    default_type text/html;
    location /t {
        echo abc;
        replace_filter 'ab|abc' X;
    }
--- request
GET /t
--- response_body
Xc
--- no_error_log
[alert]
[error]



=== TEST 4: caseless
--- config
    default_type text/html;
    location /t {
        echo abcabcaBde;
        replace_filter abCabd X i;
    }
--- request
GET /t
--- response_body
abcXe
--- no_error_log
[alert]
[error]



=== TEST 5: case sensitive (no match)
--- config
    default_type text/html;
    location /t {
        echo abcabcaBde;
        replace_filter abCabd X;
    }
--- request
GET /t
--- response_body
abcabcaBde
--- no_error_log
[alert]
[error]



=== TEST 6: 1-byte chain bufs
--- config
    default_type text/html;

    location = /t {
        echo -n a;
        echo -n b;
        echo -n a;
        echo -n b;
        echo -n a;
        echo -n c;
        echo d;
        replace_filter abac X;
    }
--- request
GET /t
--- stap2 eval: $::StapOutputChains
--- response_body
abXd
--- no_error_log
[alert]
[error]



=== TEST 7: 2-byte chain bufs
--- config
    default_type text/html;

    location = /t {
        echo -n ab;
        echo -n ab;
        echo -n ac;
        echo d;
        replace_filter abac X;
    }
--- request
GET /t
--- stap2 eval: $::StapOutputChains
--- response_body
abXd
--- no_error_log
[alert]
[error]



=== TEST 8: 3-byte chain bufs
--- config
    default_type text/html;

    location = /t {
        echo -n aba;
        echo -n bac;
        echo d;
        replace_filter abac X;
    }
--- request
GET /t
--- stap2 eval: $::StapOutputChains
--- response_body
abXd
--- no_error_log
[alert]
[error]



=== TEST 9: 3-byte chain bufs (more)
--- config
    default_type text/html;

    location = /t {
        echo -n aba;
        echo -n bac;
        echo d;
        replace_filter abacd X;
    }
--- request
GET /t
--- stap2 eval: $::StapOutputChains
--- response_body
abX
--- no_error_log
[alert]
[error]



=== TEST 10: once by default (1st char matched)
--- config
    default_type text/html;
    location /t {
        echo abcabcabde;
        replace_filter a X;
    }
--- request
GET /t
--- response_body
Xbcabcabde
--- no_error_log
[alert]
[error]



=== TEST 11: once by default (2nd char matched)
--- config
    default_type text/html;
    location /t {
        echo abcabcabde;
        replace_filter b X;
    }
--- request
GET /t
--- response_body
aXcabcabde
--- no_error_log
[alert]
[error]



=== TEST 12: global substitution
--- config
    default_type text/html;
    location /t {
        echo bbc;
        replace_filter b X g;
    }
--- request
GET /t
--- response_body
XXc
--- no_error_log
[alert]
[error]



=== TEST 13: global substitution
--- config
    default_type text/html;
    location /t {
        echo abcabcabde;
        replace_filter b X g;
    }
--- request
GET /t
--- response_body
aXcaXcaXde
--- no_error_log
[alert]
[error]



=== TEST 14: global substitution (empty captures)
--- config
    default_type text/html;
    location /t {
        echo -n abcabcabde;
        replace_filter [0-9]* X g;
    }
--- request
GET /t
--- response_body chop
XaXbXcXaXbXcXaXbXdXeX
--- no_error_log
[alert]
[error]



=== TEST 15: global substitution (empty captures, splitted)
--- config
    default_type text/html;
    location /t {
        echo -n ab;
        echo -n cab;
        echo -n c;
        echo -n abde;
        replace_filter [0-9]* X g;
    }
--- request
GET /t
--- response_body chop
XaXbXcXaXbXcXaXbXdXeX
--- no_error_log
[alert]
[error]



=== TEST 16: global substitution (\d+)
--- config
    default_type text/html;
    location /t {
        echo "hello1234, 56 world";
        replace_filter \d+ X g;
    }
--- request
GET /t
--- response_body
helloX, X world
--- no_error_log
[alert]
[error]



=== TEST 17: replace_types default to text/html
--- config
    default_type text/plain;
    location /t {
        echo abc;
        replace_filter b X;
    }
--- request
GET /t
--- response_body
abc
--- no_error_log
[alert]
[error]



==== TEST 17: custom replace_types
--- config
    default_type text/plain;
    location /t {
        echo abc;
        replace_filter b X;
        replace_filter_types text/plain;
    }
--- request
GET /t
--- response_body
aXc
--- no_error_log
[alert]
[error]



=== TEST 18: multiple replace_types settings
--- config
    default_type text/plain;
    location /t {
        echo abc;
        replace_filter b X;
        replace_filter_types text/css text/plain;
    }
--- request
GET /t
--- response_body
aXc
--- no_error_log
[alert]
[error]



=== TEST 19: trim leading spaces
--- config
    default_type text/html;
    location /a.html {
        replace_filter '^\s+' '' g;
    }
--- user_files
>>> a.html
  hello, world  
blah yeah
hello  
   baby!
     
abc
--- request
GET /a.html
--- response_body
hello, world  
blah yeah
hello  
baby!
abc
--- no_error_log
[alert]
[error]



=== TEST 20: trim trailing spaces
--- config
    default_type text/html;
    location /a.html {
        replace_filter '\s+$' '' g;
    }
--- user_files
>>> a.html
  hello, world  
blah yeah
hello  
   baby!
     
abc
--- request
GET /a.html
--- response_body chop
  hello, world
blah yeah
hello
   baby!
abc
--- no_error_log
[alert]
[error]



=== TEST 21: trim both leading and trailing spaces
--- config
    default_type text/html;
    location /a.html {
        replace_filter '^\s+|\s+$' '' g;
    }
--- user_files
>>> a.html
  hello, world  
blah yeah
hello  
   baby!
     
abc
--- request
GET /a.html
--- response_body chop
hello, world
blah yeah
hello
baby!
abc
--- no_error_log
[alert]
[error]



=== TEST 22: pure flush buf in the stream (no data)
--- config
    default_type text/html;
    location = /t {
        echo_flush;
        replace_filter 'a' 'X' g;
    }
--- request
GET /t
--- response_body chop
--- no_error_log
[alert]
[error]



=== TEST 23: pure flush buf in the stream (with data)
--- config
    default_type text/html;
    location = /t {
        echo a;
        echo_flush;
        replace_filter 'a' 'X' g;
    }
--- request
GET /t
--- stap2 eval: $::StapOutputChains
--- response_body
X
--- no_error_log
[alert]
[error]



=== TEST 24: trim both leading and trailing spaces (1 byte at a time)
--- config
    default_type text/html;
    location = /t {
        echo -n 'a';
        echo ' ';
        echo "b";
        replace_filter '^\s+|\s+$' '' g;
    }

--- stap2
F(ngx_palloc) {
    if ($size < 0) {
        print_ubacktrace()
        exit()
    }
}
--- stap3 eval: $::StapOutputChains
--- request
GET /t
--- response_body
a
b

--- no_error_log
[alert]
[error]
--- SKIP



=== TEST 25: trim both leading and trailing spaces (1 byte at a time), no \s for $
--- config
    default_type text/html;
    location = /t {
        echo -n 'a';
        echo ' ';
        echo "b";
        replace_filter '^\s+| +$' '' g;
    }

--- stap2
F(ngx_palloc) {
    if ($size < 0) {
        print_ubacktrace()
        exit()
    }
}
--- stap3 eval: $::StapOutputChains
--- request
GET /t
--- response_body
a
b

--- no_error_log
[alert]
[error]



=== TEST 26: trim both leading and trailing spaces (1 byte at a time)
--- SKIP
--- config
    default_type text/html;
    location /a.html {
        internal;
    }

    location /b.html {
        echo -n 'a';
        echo ' ';
        echo "b";
        replace_filter '^\s+|\s+$' '' g;
    }

    location = /t {
        content_by_lua '
            local res = ngx.location.capture("/a.html")
            local txt = res.body
            for i = 1, string.len(txt) do
                ngx.print(string.sub(txt, i, i))
                ngx.flush(true)
            end
        ';
        replace_filter '^\s+|\s+$' '' g;
    }
--- user_files
>>> a.html
  hello, world  
blah yeah
hello  
   baby!
     
abc

--- stap2
F(ngx_palloc) {
    if ($size < 0) {
        print_ubacktrace()
        exit()
    }
}
--- request
GET /b.html
--- response_body chop
hello, world
blah yeah
hello
baby!
abc
--- no_error_log
[alert]
[error]
