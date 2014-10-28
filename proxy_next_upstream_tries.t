#!/usr/bin/perl

# (C) Sergey Kandaurov
# (C) Nginx, Inc.

# Tests for http proxy module, proxy_next_upstream_tries
# and proxy_next_upstream_timeout directives.

###############################################################################

use warnings;
use strict;

use Test::More;

BEGIN { use FindBin; chdir($FindBin::Bin); }

use lib 'lib';
use Test::Nginx;

###############################################################################

select STDERR; $| = 1;
select STDOUT; $| = 1;

my $t = Test::Nginx->new()->has(qw/http proxy rewrite/);

$t->write_file_expand('nginx.conf', <<'EOF');

%%TEST_GLOBALS%%

daemon off;

events {
}

http {
    %%TEST_GLOBALS_HTTP%%

    upstream u {
        server 127.0.0.1:8081;
        server 127.0.0.1:8081;
        server 127.0.0.1:8081;
    }

    upstream u2 {
        server 127.0.0.1:8081;
        server 127.0.0.1:8081 backup;
        server 127.0.0.1:8081 backup;
    }

    upstream u3 {
        server 127.0.0.1:8082;
        server 127.0.0.1:8082;
        server 127.0.0.1:8082;
    }

    upstream u4 {
        server 127.0.0.1:8082;
        server 127.0.0.1:8082 backup;
        server 127.0.0.1:8082 backup;
    }

    server {
        listen       127.0.0.1:8080;
        server_name  localhost;

        proxy_next_upstream http_404;
        proxy_intercept_errors on;
        error_page 404 /404;

        location /tries {
            proxy_pass http://u;
            proxy_next_upstream_tries 2;
        }

        location /tries/backup {
            proxy_pass http://u2;
            proxy_next_upstream_tries 2;
        }

        location /tries/resolver {
            resolver 127.0.0.1:8083;
            resolver_timeout 1s;

            proxy_pass http://$host:8081/backend;
            proxy_next_upstream_tries 2;
        }

        location /timeout {
            proxy_pass http://u3;
            proxy_next_upstream_timeout 1500ms;
        }

        location /timeout/backup {
            proxy_pass http://u4;
            proxy_next_upstream_timeout 1500ms;
        }

        location /timeout/resolver {
            resolver 127.0.0.1:8083;
            resolver_timeout 1s;

            proxy_pass http://$host:8082/backend;
            proxy_next_upstream_timeout 1500ms;
        }

        location /404 {
            return 200 x${upstream_status}x;
        }
    }
}

EOF

$t->run_daemon(\&http_daemon, 8081);
$t->run_daemon(\&http_daemon, 8082);
$t->run_daemon(\&dns_daemon, 8083, $t);
$t->try_run('no proxy_next_upstream_tries')->plan(6);

$t->waitforsocket('127.0.0.1:8081');
$t->waitforsocket('127.0.0.1:8082');
$t->waitforfile($t->testdir . '/8083');

###############################################################################

like(http_get('/tries'), qr/x404, 404x/, 'tries');
like(http_get('/tries/backup'), qr/x404, 404x/, 'tries backup');

TODO: {
local $TODO = 'not yet' unless $t->has_version('1.7.7');

like(http_get('/tries/resolver'), qr/x404, 404x/, 'tries resolved');

}

# two tries fit into 1.5s

like(http_get('/timeout'), qr/x404, 404x/, 'timeout');
like(http_get('/timeout/backup'), qr/x404, 404x/, 'timeout backup');

TODO: {
local $TODO = 'not yet' unless $t->has_version('1.7.7');

like(http_get('/timeout/resolver'), qr/x404, 404x/, 'timeout resolved');

}

###############################################################################

sub http_daemon {
	my ($port) = @_;

	my $server = IO::Socket::INET->new(
		Proto => 'tcp',
		LocalHost => '127.0.0.1',
		LocalPort => $port,
		Listen => 5,
		Reuse => 1
	)
		or die "Can't create listening socket: $!\n";

	local $SIG{PIPE} = 'IGNORE';

	while (my $client = $server->accept()) {
		$client->autoflush(1);

		my $headers = '';

		while (<$client>) {
			$headers .= $_;
			last if (/^\x0d?\x0a?$/);
		}

		next if $headers eq '';

		if ($port == 8082) {
			Test::Nginx::log_core('||', "$port: sleep(1)");
			select undef, undef, undef, 1;
		}

		Test::Nginx::log_core('||', "$port: response, 404");
		print $client <<EOF;
HTTP/1.1 404 Not Found
Connection: close

EOF

	} continue {
		close $client;
	}
}

sub reply_handler {
	my ($recv_data) = @_;

	my (@name, @rdata);

	use constant NOERROR	=> 0;
	use constant A		=> 1;
	use constant IN 	=> 1;

	# default values

	my ($hdr, $rcode, $ttl) = (0x8180, NOERROR, 3600);

	# decode name

	my ($len, $offset) = (undef, 12);
	while (1) {
		$len = unpack("\@$offset C", $recv_data);
		last if $len == 0;
		$offset++;
		push @name, unpack("\@$offset A$len", $recv_data);
		$offset += $len;
	}

	$offset -= 1;
	my ($id, $type, $class) = unpack("n x$offset n2", $recv_data);

	@rdata = map { rd_addr($ttl, '127.0.0.1') } (1 .. 3) if $type == A;

	$len = @name;
	pack("n6 (w/a*)$len x n2", $id, $hdr | $rcode, 1, scalar @rdata,
		0, 0, @name, $type, $class) . join('', @rdata);
}

sub rd_addr {
	my ($ttl, $addr) = @_;

	my $code = 'split(/\./, $addr)';

	return pack 'n3N', 0xc00c, A, IN, $ttl if $addr eq '';

	pack 'n3N nC4', 0xc00c, A, IN, $ttl, eval "scalar $code", eval($code);
}

sub dns_daemon {
	my ($port, $t) = @_;

	my ($data, $recv_data);
	my $socket = IO::Socket::INET->new(
		LocalAddr    => '127.0.0.1',
		LocalPort    => $port,
		Proto        => 'udp',
	)
		or die "Can't create listening socket: $!\n";

	# signal we are ready

	open my $fh, '>', $t->testdir() . '/' . $port;
	close $fh;

	while (1) {
		$socket->recv($recv_data, 65536);
		$data = reply_handler($recv_data);
		$socket->send($data);
	}
}

###############################################################################
