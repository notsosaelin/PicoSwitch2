Noticed a bug.

work flow to produce bug

boot into procon 2 mode -> Swap to config mode -> connect do a thing (change color or flash new amiibo) -> change back to procon 2 mode -> change back to config -> change back to procon 2 mode now the input stops working, I have to unplug and replug the adapter. they should be able to freely swap in and out of config mode without stalling or locking up

When trying to read a Kirby Air Riders amiibo flashed to the adapter, 1 of 2 things happen

in the System menu where you normally can assign owner and amiibo nickname -> It does not read the amiibo at all, we would need a real amiibo to test and confirm if that's expected

in Kirby Air Rider, it does start to read the amiibo when i open up the amiibo tab, but it returns error 2115-0088. https://www.reddit.com/r/kirbyairriders/comments/1p3637a/bandana_waddle_dee_amiibo_not_working/ this error thread references this exact error happening with real amiibos so we may have to test over uart exactly what happens and alter our approach of presentation to match. 

Other than that the virtual amiibo module is i want to say like 90 percent done.