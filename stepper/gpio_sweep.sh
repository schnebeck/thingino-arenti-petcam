#!/bin/sh
# Export every GPIO not already in our known pin table as input, then watch
# for value changes while the button is pressed repeatedly.

KNOWN="0 10 11 18 40 41 42 43 45 46 49 50 51 52 53 54 57 59 60"

exported=""
for p in $(seq 0 95); do
    case " $KNOWN " in *" $p "*) continue;; esac
    if [ ! -d /sys/class/gpio/gpio$p ]; then
        if echo $p > /sys/class/gpio/export 2>/dev/null; then
            exported="$exported $p"
        fi
    fi
done

for p in $exported; do
    echo in > /sys/class/gpio/gpio$p/direction 2>/dev/null
done

echo "watching pins:$exported"
n=$(echo $exported | wc -w)
echo "count: $n"

prev=""
for p in $exported; do
    v=$(cat /sys/class/gpio/gpio$p/value 2>/dev/null)
    prev="$prev $p=$v"
done
echo "initial:$prev"
echo "press the button repeatedly now, over the next 20s"

for i in $(seq 1 80); do
    cur=""
    for p in $exported; do
        v=$(cat /sys/class/gpio/gpio$p/value 2>/dev/null)
        cur="$cur $p=$v"
    done
    if [ "$cur" != "$prev" ]; then
        for p in $exported; do
            oldv=$(echo "$prev" | tr ' ' '\n' | grep "^$p=" | cut -d= -f2)
            newv=$(echo "$cur" | tr ' ' '\n' | grep "^$p=" | cut -d= -f2)
            if [ "$oldv" != "$newv" ]; then
                echo "CHANGE gpio$p: $oldv -> $newv (tick $i)"
            fi
        done
    fi
    prev="$cur"
    sleep 0.25
done

for p in $exported; do
    echo $p > /sys/class/gpio/unexport 2>/dev/null
done
echo "done, unexported all"
