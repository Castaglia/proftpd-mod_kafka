package ProFTPD::Tests::Modules::mod_kafka::tls;

use lib qw(t/lib);
use base qw(ProFTPD::TestSuite::Child);
use strict;

use File::Path qw(mkpath);
use File::Spec;
use IO::Handle;

use ProFTPD::TestSuite::FTP;
use ProFTPD::TestSuite::Utils qw(:auth :config :features :running :test :testsuite);

$| = 1;

my $order = 0;

my $TESTS = {
  kafka_tls_log_on_event => {
    order => ++$order,
    test_class => [qw(forking)],
  },

};

sub new {
  return shift()->SUPER::new(@_);
}

sub list_tests {
  # Check for the required Perl modules:
  #
  #  Kafka

  my $required = [qw(
    JSON
    Kafka
  )];

  foreach my $req (@$required) {
    eval "use $req";
    if ($@) {
      print STDERR "\nWARNING:\n + Module '$req' not found, skipping all tests\n";

      if ($ENV{TEST_VERBOSE}) {
        print STDERR "Unable to load $req: $@\n";
      }

      return qw(testsuite_empty_test);
    }
  }

  return testsuite_get_runnable_tests($TESTS);
}

sub get_kafka_host {
  my $kafka_host = 'localhost';

  if (defined($ENV{KAFKA_HOST})) {
    $kafka_host = $ENV{KAFKA_HOST};
  }

  return $kafka_host;
}

sub kafka_topic_getall {
  my $name = shift;

  require Kafka;
  require Kafka::Connection;
  require Kafka::Consumer;

  my $kafka_host = get_kafka_host();
  my $kafka = Kafka::Connection->new(host => $kafka_host);
  my $consumer = Kafka::Consumer->new(Connection => $kafka);

  my $msgs = $consumer->fetch($name, 0, 0, $Kafka::DEFAULT_MAX_BYTES);
  $consumer = undef;
  $kafka->close;
  $kafka = undef;

  return $msgs;
}

# There is no easy way to purge a topic in Kafka; we thus need to generate
# unique topic names for each test.
sub get_topic_name {
  my $name = '';

  for (1..16) {
    # Add 96 to get into the ASCII range, past punctuation
    $name .= chr(int(rand(26) + 97));
  }

  return $name;
}

# Tests

sub kafka_tls_log_on_event {
  my $self = shift;
  my $tmpdir = $self->{tmpdir};
  my $setup = test_setup($tmpdir, 'kafka');

  my $fmt_name = 'mod_kafka';
  my $topic = $fmt_name;
  kafka_topic_getall($topic);

  my $kafka_host = get_kafka_host();

  my $client_cert = File::Spec->rel2abs("$ENV{PROFTPD_TEST_DIR}/tests/t/etc/modules/mod_tls/client-cert.pem");
  my $ca_cert = File::Spec->rel2abs("$ENV{PROFTPD_TEST_DIR}/tests/t/etc/modules/mod_tls/ca-cert.pem");

  my $config = {
    PidFile => $setup->{pid_file},
    ScoreboardFile => $setup->{scoreboard_file},
    SystemLog => $setup->{log_file},
    TraceLog => $setup->{log_file},
    Trace => 'jot:20 kafka:20',

    AuthUserFile => $setup->{auth_user_file},
    AuthGroupFile => $setup->{auth_group_file},
    AuthOrder => 'mod_auth_file.c',

    IfModules => {
      'mod_delay.c' => {
        DelayEngine => 'off',
      },

      # Note: we need to use arrays here, since order of directives matters.
      'mod_kafka.c' => [
        'KafkaEngine on',
        "KafkaBroker $kafka_host:9093",
        "KafkaProperty ssl.ca.location $ca_cert",
        "KafkaProperty ssl.certification.location $client_cert",
        "KafkaProperty ssl.key.location $client_cert",

        "KafkaLog $setup->{log_file}",
        "LogFormat $fmt_name \"%A %a %b %c %D %d %E %{epoch} %F %f %{gid} %g %H %h %I %{iso8601} %J %L %l %m %O %P %p %{protocol} %R %r %{remote-port} %S %s %T %t %U %u %{uid} %V %v %{version}\"",
        "KafkaLogOnEvent ALL $fmt_name",
      ],
    },
  };

  my ($port, $config_user, $config_group) = config_write($setup->{config_file},
    $config);

  # Open pipes, for use between the parent and child processes.  Specifically,
  # the child will indicate when it's done with its test by writing a message
  # to the parent.
  my ($rfh, $wfh);
  unless (pipe($rfh, $wfh)) {
    die("Can't open pipe: $!");
  }

  my $ex;

  # Fork child
  $self->handle_sigchld();
  defined(my $pid = fork()) or die("Can't fork: $!");
  if ($pid) {
    eval {
      # Allow for server startup
      sleep(1);

      my $client = ProFTPD::TestSuite::FTP->new('127.0.0.1', $port);
      $client->login($setup->{user}, $setup->{passwd});

      my $resp_code = $client->response_code();
      my $resp_msg = $client->response_msg(0);

      my $expected = 230;
      $self->assert($expected == $resp_code,
        "Expected response code $expected, got $resp_code");

      $expected = "User $setup->{user} logged in";
      $self->assert($expected eq $resp_msg,
        "Expected response message '$expected', got '$resp_msg'");

      $client->quit();
    };
    if ($@) {
      $ex = $@;
    }

    $wfh->print("done\n");
    $wfh->flush();

  } else {
    eval { server_wait($setup->{config_file}, $rfh) };
    if ($@) {
      warn($@);
      exit 1;
    }

    exit 0;
  }

  # Stop server
  server_stop($setup->{pid_file});
  $self->assert_child_ok($pid);

  eval {
    # Allow for propagation time
    sleep(2);

    my $data = kafka_topic_getall($topic);

    my $nrecords = scalar(@$data);
    $self->assert($nrecords >= 4 || $nrecords >= 5,
      "Expected at least 4-5 records, got $nrecords");

    require JSON;
    my $json = $data->[3]->{payload};
    my $record = decode_json($json);

    my $expected = $setup->{user};
    $self->assert($record->{user} eq $expected,
      "Expected user '$expected', got '$record->{user}'");

    $expected = '127.0.0.1';
    $self->assert($record->{remote_ip} eq $expected,
      "Expected remote IP '$expected', got '$record->{remote_ip}'");
  };
  if ($@) {
    $ex = $@;
  }

  test_cleanup($setup->{log_file}, $ex);
}

1;
