public final class EntryGateTest {
  static int n;
  static void eq(int actual,int expected,String name){if(actual!=expected)throw new AssertionError(name);n++;}
  public static void main(String[] args){
    EntryGate g=new EntryGate();
    eq(g.update(false,false,false,0),0,"outside ignored");
    eq(g.update(true,false,false,1000),0,"one sample ignored");
    eq(g.update(true,false,false,2000),1,"two samples start");
    eq(g.update(true,false,false,3000),0,"no retry while same page");
    eq(g.update(false,false,true,4000),0,"departure debounce");
    eq(g.update(false,false,true,6999),0,"under three seconds");
    eq(g.update(false,false,true,7000),2,"idle cancellation request only");
    eq(g.update(true,true,true,8000),0,"owned transfer prevents launch");
    eq(g.update(true,false,false,9000),1,"new entry waits for owned recovery then starts");
    eq(g.update(false,false,false,10000),0,"leave starts rearm");
    eq(g.update(false,false,false,13000),0,"rearm does not start outside");
    eq(g.update(true,false,false,14000),0,"new entry sample one");
    eq(g.update(true,false,false,15000),1,"new entry sample two");
    EntryGate b=new EntryGate();
    eq(b.update(true,true,false,0),0,"foreign radio lock blocks");
    eq(b.update(true,false,false,1000),0,"foreign session ending no automatic takeover");
    EntryGate u=new EntryGate();
    eq(u.update(true,false,false,0),0,"new gate sample");u.invalid();
    eq(u.update(true,false,false,1000),0,"error breaks stability");
    eq(u.update(true,false,false,2000),1,"reestablished stable state");
    EntryGate same=new EntryGate();same.update(true,false,false,0);same.update(true,false,false,1000);
    eq(same.update(true,true,true,2000),0,"same entry remains consumed during owned session");
    eq(same.update(true,false,false,3000),0,"same entry finish cannot restart");
    EntryGate locked=new EntryGate();locked.update(true,false,false,0);locked.update(true,false,false,1000);
    eq(locked.update(false,false,true,true,2000),0,"lock departure debounce");
    eq(locked.update(false,false,true,true,5000),2,"lock may request idle stop only");
    eq(locked.update(true,true,true,true,6000),0,"unlock same page with active session");
    eq(locked.update(true,true,false,false,7000),0,"unlock never rearms consumed entry");
    System.out.println("ENTRY_GATE_TESTS="+n);
  }
}
