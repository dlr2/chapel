#!/usr/bin/env perl

while (<>) {
    if (m/Compiler Command : .*\s(.*\.chpl)$/) {
        $program = $1;
    } elsif (m/Execution Command:.*tmp\.\.\-(.*?)\./) {
        $path = $1;
        $path =~ s/\-/\//g;
        $program = "$path/$program";
    } elsif (m/Memory Statistics/) {
        $memstats = 1;
    } elsif (m/Leaked Memory Report/) {
        $memleaks = 1;
    } elsif ($memstats == 1) {
        if (m/==============================================================/) {
            $memstats = 2;
        }
    } elsif ($memstats == 2) {
        if (m/==============================================================/) {
            $memstats = 0;
        } elsif (m/Current Allocated Memory\s*(\d*)$/) {
            $totalLeakedMemory += $1;
        } elsif (m/Maximum Simultaneous Allocated Memory\s*(\d*)$/) {
            $maximumAllocatedMemory += $1;
        } elsif (m/Total Allocated Memory\s*(\d*)$/) {
            $totalAllocatedMemory += $1;
        } elsif (m/Total Freed Memory\s*(\d*)$/) {
            $totalFreedMemory += $1;
        }
    } elsif ($memleaks == 1) {
        if (m/==============================================================/) {
            $memleaks = 2;
        }
    } elsif ($memleaks == 2) {
        if (m/==============================================================/) {
            $memleaks = 3;
        }
    } elsif ($memleaks == 3) {
        if (m/==============================================================/) {
            $memleaks = 0;
        } elsif (m/(\d*)\s*(\d*)\s*(.*)$/) {
            if (($3 eq "string copy data") ||
                ($3 eq "glom strings data") ||
                ($3 eq "string concat data") ||
                ($3 eq "set wide string") ||
                ($3 eq "get wide string") ||
                ($3 eq "string strided select data")) {
            } else {
                $TP{$program} += $2;
            }
            $TZ{$3} += $2; # table of size of leaked memory
            $TN{$3} += $1; # table of number of leaked allocations
        }
    }
}

printf("=================\n");
printf("Memory Statistics\n");
printf("==============================================================\n");
printf("Total Leaked Memory:        %12d\n", $totalLeakedMemory);
printf("Total Allocated Memory:     %12d\n", $totalAllocatedMemory);
printf("Total Freed Memory:         %12d\n", $totalFreedMemory);
printf("Maximum Allocated Memory:   %12d\n", $maximumAllocatedMemory);
printf("Number of Tests with Leaks: %12d\n", scalar keys %TP);
printf("==============================================================\n");
printf("\n");
printf("====================\n");
printf("Leaked Memory Report\n");
printf("==============================================================\n");
printf("Number of leaked allocations\n");
printf("              Total leaked memory (bytes)\n");
printf("                           Description of allocation\n");
printf("==============================================================\n");

@descs = sort { $TZ{$b} <=> $TZ{$a} } keys %TZ;

foreach $desc (@descs) {
    printf("%12d  %12d  $desc\n", $TN{$desc}, $TZ{$desc});
}

printf("==============================================================\n");
printf("\n");
printf("=======================\n");
printf("Memory Leaking Programs\n");
printf("   (ignoring strings)\n");
printf("==============================================================\n");
printf("Total leaked memory (bytes)\n");
printf("              Program\n");
printf("==============================================================\n");

@programs = sort { $TP{$b} <=> $TP{$a} } keys %TP;

foreach $program (@programs) {
    printf("%12d  $program\n", $TP{$program});
}

printf("==============================================================\n");
