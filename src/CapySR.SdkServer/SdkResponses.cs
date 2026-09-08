namespace CapySR.SdkServer;

// the client only checks retcode and pulls the token through; none of this is verified
public static class SdkResponses
{
    public static readonly object Login = new
    {
        data = new
        {
            account = new
            {
                area_code = "**",
                email = "capybara@capysr.local",
                country = "US",
                is_email_verify = "1",
                token = "capysr",
                uid = "1337",
            },
            device_grant_required = false,
            reactivate_required = false,
            realperson_required = false,
            safe_mobile_required = false,
        },
        message = "OK",
        retcode = 0,
    };

    public static readonly object Granter = new
    {
        data = new
        {
            account_type = 1,
            combo_id = "1337",
            combo_token = "9065ad8507d5a1991cb6fddacac5999b780bbd92",
            data = "{\"guest\":false}",
            heartbeat = false,
            open_id = "1337",
        },
        message = "OK",
        retcode = 0,
    };

    public static readonly object RiskyCheck = new
    {
        data = new
        {
            id = "06611ed14c3131a676b19c0d34c0644b",
            action = "ACTION_NONE",
            geetest = (object?)null,
        },
        message = "OK",
        retcode = 0,
    };

    public static readonly object PassportLogin = new
    {
        data = new
        {
            token = new { token = "capysr", token_type = 1 },
            user_info = new
            {
                aid = "1337",
                mid = "1337",
                is_email_verify = 1,
                area_code = "**",
                country = "US",
                is_adult = 1,
                email = "capybara@capysr.local",
            },
        },
        message = "OK",
        retcode = 0,
    };

    public static readonly object PassportVerify = new
    {
        data = new
        {
            tokens = new[] { new { token = "capysr", token_type = 1 } },
            user_info = new
            {
                aid = "1337",
                mid = "1337",
                is_email_verify = 1,
                area_code = "**",
                country = "US",
                is_adult = 1,
                email = "capybara@capysr.local",
            },
        },
        message = "OK",
        retcode = 0,
    };
}
