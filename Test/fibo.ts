#
# simple fibonacci demo
#

# calculate fibo(n)
# parameter in n
# returns result in r

var n=0
var r=0

proc fibo {
  if (n<2) {
    r=n
  } else {
    var saven=n  # save our parameter for recursive call
    n=n-1
    fibo
    var a=r
    n=saven-2
    fibo
    r=r+a
    n=saven
  }
}

var i=1
while i<=12 {
  n=i; fibo
  print "fibo(",i,") = ",r
  i=i+1
}
