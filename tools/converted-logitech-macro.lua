y = 0
d = 0
s = 0
x = 0
q_key = 0
count = 0
runtime = 0
press_time_point = 0
release_time_point = 0
random_press_time = 0
random_release_time = 0
rshift_pressed = 0
can_hold_breath = 1
left_is_pressed = 0
remain_x = 0
remain_y = 0
can_aim = 1

function OnEvent(event, arg)
    if event == "pressed" and (arg == 1 or arg == 3) then
        --ClearLog()
    end
    DebugLog("arg=%s", tostring(arg))

    if event == "pressed" and arg == "f15" then
        if d < 20 then
            d = d + 1
        end
        if s == 0 then
            s = 1
        end
        DebugLog("s=%d d=%d", s, d)
    end

    if event == "pressed" and arg == "f14" then
        if d > 0 then
            d = d - 1
        end
        if s == 0 then
            s = 1
        end
        DebugLog("s=%d d=%d", s, d)
    end

    if event == "pressed" and arg == "f16" then
        if s == 0 then
            s = 1
        else
            s = 0
        end
        d = 0
        DebugLog("s=%d", s)
    end

    if event == "pressed" and arg == "f18" then
        if can_aim == 1 then
            can_aim = 2
        else
            can_aim = 1
        end
        DebugLog("can_aim=%d", can_aim)
    end

    if event == "pressed" and arg == 3 and can_aim == 2 then
        aim(1)
    end

    if event == "released" and arg == 3 and can_aim == 2 then
        aim(0)
    end

    if event == "released" and arg == 1 then
        count = 0
        remain_x = 0
        remain_y = 0
    end

    if event == "pressed" and (arg == 1 or arg == 3) then
        runtime = 0
        count = 0
        while IsPressed(1) and IsPressed(3) do
            if IsPressed("lctrl") then
                tmp_d = math.floor(d * 1)
            else
                tmp_d = d
            end
            y = tmp_d
            sleep_time = 17
            x = 0

            if count < 40 then
                if can_aim == 1 and count == 1 then
                    aim(1)
                end
                if count < 5 then
                    tmp_y = 0
                else
                    tmp_y = math.floor(y + 1.0)
                end
            else
                tmp_y = math.floor(y + 0)
            end

            MoveMouse(x, tmp_y)
            Sleep2(sleep_time)
            count = count + 1
            runtime = runtime + sleep_time
	    --DebugLog("move x=%d,tmp_y)=%d,count=%d",x,tmp_y),count)
        end

        if can_aim == 1 and count > 0 then
            aim(0)
        end
        count = 0
        runtime = 0
    end

    if event == "pressed" and arg == "f13" then
        count = 0
        runtime = 0
        while IsPressed("f13") do
            if IsPressed("lctrl") then
                tmp_d = math.floor(d * 1)
            else
                tmp_d = d
            end
            y = tmp_d
            x = 0

            if can_aim == 1 and count == 1 then
                aim(1)
            end
            sleep_time = 14
            click(20, 20)

            if IsPressed(3) and s == 1 then
                MoveMouse(x, y)
            end
            Sleep2(sleep_time)
            count = count + 1
            runtime = runtime + sleep_time
        end

        if can_aim == 1 then
            aim(0)
        end
        mouse(1, 0)
        count = 0
        runtime = 0
        release_time_point = 0
        press_time_point = 0
        left_is_pressed = 0
    end

    if event == "pressed" and arg == "f17" then
        if s == 0 then
            s = 1
        end
        d = 4
        x = 0
        can_hold_breath = 1
        DebugLog("x=%d d=%d can_hold_breath=%d", x, d, can_hold_breath)
    end
end

function MoveMouse(x, y)
    move_x = math.floor(x)
    if remain_x == 0 then
        if move_x < x then
            remain_x = 1
        end
    else
        move_x = move_x + remain_x
        remain_x = 0
    end

    move_y = math.floor(y) - 1
    if move_y < 0 then
        move_y = 0
    end

    if remain_y == 0 then
        if move_y < y then
            remain_y = 1
        end
    else
        move_y = move_y + remain_y
        remain_y = 0
    end

    DebugLog("move_x=%d move_y=%d remain_y=%d", move_x, move_y, remain_y)
    move(move_x, move_y)
end

function breath(hold)
    if can_hold_breath == 1 then
        rshift_pressed = hold
        if hold == 1 then
            keydown("num0")
            DebugLog("num0 pressed")
        else
            keyup("num0")
            DebugLog("num0 released")
        end
    end
end

function aim(state)
    if can_aim ~= 0 then
        rshift_pressed = state
        if state == 1 then
            keydown("insert")
            mouse(5, 1)
            DebugLog("insert pressed")
        else
            keyup("insert")
            mouse(5, 0)
            DebugLog("insert released")
        end
    end
end

function click(press_time, release_time)
    if random_release_time == 0 then
        random_release_time = release_time + math.random(0, 10)
        DebugLog("random_release_time=%d", random_release_time)
    end

    if runtime >= press_time_point + random_press_time and left_is_pressed == 1 then
        release_time_point = runtime
        left_is_pressed = 0
        mouse(1, 0)
        DebugLog("released")

        random_release_time = release_time + math.random(0, 10)
        DebugLog("random_release_time=%d", random_release_time)
    end

    if (runtime == 0 or runtime >= release_time_point + random_release_time) and left_is_pressed == 0 then
        left_is_pressed = 1
        mouse(1, 1)
        press_time_point = runtime
        DebugLog("pressed")

        random_press_time = press_time + math.random(0, 15)
        DebugLog("random_press_time=%d", random_press_time)
    end
end

function loop_click(loop)
    if count == 0 or count % loop == 0 then
        mouse(1, 1)
        count2 = count
        count3 = count
    end

    if count == count2 + 2 then
        mouse(1, 0)
        count2 = 0
    end

    if count == count3 + loop - 1 then
        count3 = 0
    end
    DebugLog("count=%d count2=%d count3=%d loop=%d", count, count2, count3, loop)
end

function Sleep2(time)
    Sleep(math.max(1, math.floor(time)))
end
