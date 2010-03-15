#!/usr/bin/env perl

use File::Basename;

$utildirname = dirname($0);

$preset_timers = $ENV{'CHPL_TIMERS'};

if ($preset_timers eq "") {
  $targplatform = `$utildirname/platform.pl --target`; chomp($targplatform);
  if ($targplatform eq "xmt") {
    $timers = 'xmt';
  } else {
    $timers = 'generic';
  }
} else {
  $timers = $preset_timers;
}

print "$timers\n";
exit(0);
