/*
 *  Check correctness of sort routines. Output nothing if correct.
 */

use Sort;
use Random;

proc main() {

  // Comparators
  const absKey = new AbsKeyCmp(),
        absComp = new AbsCompCmp(),
        absKeyComp = new AbsKeyCompCmp(),
        revAbsKey = new ReverseComparator(absKey),
        revAbsComp = new ReverseComparator(absComp),
        tupleKey = new TupleCmp();

  // Pre-sorted arrays paired with comparators
  var tests = (
                ([-4, -1, 2, 3], defaultComparator),
                (['Brad', 'anthony', 'ben', 'david'], defaultComparator),
                ([-1, 2, 3, -4], absKey),
                ([-1, 2, 3, -4], absComp),
                ([-1, 2, 3, -4], absKeyComp),
                ([ 3, 2, -1, -4], reverseComparator),
                ([ -4, 3, 2, -1], revAbsKey),
                ([ -4, 3, 2, -1], revAbsComp),
                ([-4, -1, 2, 3], tupleKey)
              );


  /* Correctness test isSorted(), so that it can test the remaining sorts*/
  {
    var isSortedWorks= true;

    for param i in 1..tests.size {
      var (arr, cmp) = tests(i);

      if !isSorted(arr, comparator=cmp) {
        writeln('isSorted failed to sort array:');
        writeln(arr);
        writeln('with comparator:');
        writeln(cmp.name());
        isSortedWorks = false;
      }
    }

    if !isSortedWorks then
      halt('isSorted() failed - cannot test remaining sort functions');
  }

  /* Correctness tests for sort routines */
  {
    for param i in 1..tests.size {
      var (arr, cmp) = tests(i);
      resetArray(arr, cmp);
      sort(arr, comparator=cmp);
      if !checkSort(arr, cmp) then
        writeln('  for sort() function.\n');
    }

    for param i in 1..tests.size {
      var (arr, cmp) = tests(i);
      resetArray(arr, cmp);
      bubbleSort(arr, comparator=cmp);
      if !checkSort(arr, cmp) then
        writeln('  for bubbleSort() function.\n');
    }

    for param i in 1..tests.size {
      var (arr, cmp) = tests(i);
      resetArray(arr, cmp);
      insertionSort(arr, comparator=cmp);
      if !checkSort(arr, cmp) then
        writeln('  for insertionSort() function.\n');
    }

    for param i in 1..tests.size {
      var (arr, cmp) = tests(i);
      resetArray(arr, cmp);
      quickSort(arr, comparator=cmp);
      if !checkSort(arr, cmp) then
        writeln('  for quickSort() function.\n');
    }

    for param i in 1..tests.size {
      var (arr, cmp) = tests(i);
      resetArray(arr, cmp);
      heapSort(arr, comparator=cmp);
      if !checkSort(arr, cmp) then
        writeln('  for heapSort() function.\n');
    }

    for param i in 1..tests.size {
      var (arr, cmp) = tests(i);
      resetArray(arr, cmp);
      selectionSort(arr, comparator=cmp);
      if !checkSort(arr, cmp) then
        writeln('  for selectionSort() function.\n');
    }

    for param i in 1..tests.size {
      var (arr, cmp) = tests(i);
      resetArray(arr, cmp);
      mergeSort(arr, comparator=cmp);
      if !checkSort(arr, cmp) then
        writeln('  for mergeSort() function.\n');
    }
  }

  /* Minimal tests for deprecated sorts */
  // TODO - remove these for 1.15
  {
    var Arr = [3, -2, 1, 4];

    resetArray(Arr);
    BubbleSort(Arr);
    if !checkSort(Arr) then
      writeln('  for BubbleSort() function.\n');

    resetArray(Arr);
    InsertionSort(Arr);
    if !checkSort(Arr) then
      writeln('  for InsertionSort() function.\n');

    resetArray(Arr);
    QuickSort(Arr);
    if !checkSort(Arr) then
      writeln('  for QuickSort() function.\n');

    resetArray(Arr);
    SelectionSort(Arr);
    if !checkSort(Arr) then
      writeln('  for SelectionSort() function.\n');

    resetArray(Arr);
    HeapSort(Arr);
    if !checkSort(Arr) then
      writeln('  for HeapSort() function.\n');

    resetArray(Arr);
    MergeSort(Arr);
    if !checkSort(Arr) then
      writeln('  for MergeSort() function.\n');
  }
}


/* Checks if array is sorted with provided comparator */
proc checkSort(arr, cmp) {
  var result = true;

  // Check result
  if !isSorted(arr, cmp) {
      writeln('failed to sort array:');
      writeln(arr);
      writeln('with comparator:');
      writeln(cmp.name());
      result = false;
  }
  return result;
}


/* Checks if array is sorted */
proc checkSort(arr) {
  var result = true;

  // Check result
  if !isSorted(arr) {
      writeln('failed to sort array:');
      writeln(arr);
      result = false;
  }
  return result;
}


/* Reset array by shuffling until it's not sorted */
proc resetArray(ref array) {
  while isSorted(array) {
    Random.shuffle(array);
  }
}


/* Reset array by shuffling until it's not sorted with provided comparator */
proc resetArray(ref array, cmp) {
  while isSorted(array, cmp) {
    Random.shuffle(array);
  }
}


/* Enables more useful error messages */
proc DefaultComparator.name() { return 'DefaultComparator';}
proc ReverseComparator.name() { return 'ReverseComparator';}


/* Key Sort by absolute value */
record AbsKeyCmp {
  proc key(a) { return abs(a); }
  proc name() { return 'AbsKeyCmp'; }
}


/* Compare Sort by absolute value */
record AbsCompCmp {
  proc compare(a, b) { return abs(a) - abs(b); }
  proc name() { return 'AbsCompCmp'; }
}


/* Key method should take priority over compare method */
record AbsKeyCompCmp {
  proc key(a) { return abs(a); }
  proc compare(a, b) { return a - b; }
  proc name() { return 'AbsKeyCompCmp'; }
}


/* Key method can return a non-numerical/string type, such as tuple */
record TupleCmp {
  proc key(a) { return (a, a); }
  proc name() { return 'TupleCmp'; }
}

