// The client side of the resource, in each player's game.
public class Main : Script
{
    public override void OnStart()
    {
        Hud.ShowNotification("my-resource is running");
        Events.ToServer("my-resource:hello");
    }
}
