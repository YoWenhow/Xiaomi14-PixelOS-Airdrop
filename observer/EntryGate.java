/** Pure foreground-edge policy, shared by the observer and its host tests. */
final class EntryGate {
  static final int NONE=0,START=1,CANCEL_IF_IDLE=2;
  private int stable;
  private boolean consumed;
  private long away=-1;
  int update(boolean quickShare,boolean busy,boolean ownSession,long now){
    return update(quickShare,true,busy,ownSession,now);
  }
  int update(boolean quickShare,boolean canRearm,boolean busy,boolean ownSession,long now){
    if(quickShare){
      away=-1;stable++;
      // A newly reopened page may wait for our preceding session to recover.
      // A foreign/manual session must not be adopted or followed automatically.
      if(busy&&!ownSession)consumed=true;
      if(stable>=2&&!consumed&&!busy){consumed=true;return START;}
    }else{
      stable=0;if(away<0)away=now;
      // Locking/turning the screen off is not opening a new sharing page.
      if(now-away>=3000){if(canRearm)consumed=false;if(ownSession)return CANCEL_IF_IDLE;}
    }
    return NONE;
  }
  void invalid(){stable=0;}
}
