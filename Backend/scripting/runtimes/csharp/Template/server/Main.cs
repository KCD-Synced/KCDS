// The server side of the resource. Every class that derives from Script starts when the resource
// does: its OnStart runs once, and what it registers (handlers, timers, exports) ends with the
// resource.
public class Main : Script
{
    public override void OnStart()
    {
        Log.Info("my-resource started on the server");
        Events.On("playerConnect", (Player player) =>
        {
            Chat.SendToPlayer(player, "Welcome, " + player.Nickname + "!");
        });
        Events.FromClient("my-resource:hello", (Player sender) =>
        {
            Log.Info(sender.Nickname + " says hello");
        });
    }
}
