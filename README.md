# qlens
QLens software package beta version

# change log

Upgrades in new version (Nov. 11, 2017):

1. General lens parameter anchoring

	General lens parameter anchoring has been implemented, so that you can now anchor a lens parameter to any other lens parameter. To demonstrate this, suppose our first lens is entered as follows:

	fit lens alpha 5 1 0 0.8 30 0 0  
	1 1 0 1 1 1 1

	so that this now becomes listed as lens "0".

	  a) Anchor type 1: Now suppose I add another model, e.g. a kappa multipole, where I want the angle to always be equal to that of lens 0. Then I enter this as

	  fit lens kmpole 0.1 2 anchor=0,4 0 0  
	  1 0 0 1 1

	  The "anchor=0,4" means we are anchoring this parameter (the angle) to lens 0, parameter 4 which is the angle of the first lens (remember the first parameter is indexed as zero!). The vary flag must be turned off for the parameter being anchored, or else qlens will complain.

	  NOTE: Keep in mind that as long as you use the correct format, qlens will not complain no matter how absurd the choice of anchoring is; so make sure you have indexed it correctly! To test it out, you can use "lens update ..." to update the lens you are anchoring to, and make sure that the anchored parameter changes accordingly.

	  b) Anchor type 2: Suppose I want to add a model where I want a parameter to keep the same *ratio* with a parameter in another lens that I started with. You can do this using the following format:

	  fit lens alpha 2.5/anchor=0,0 1 0 0.8 30 0 0  
	  1 0 0 1 1 1 1

	  The "2.5/anchor=0,0" enters the initial value in as 2.5, and since this is half of the parameter we are anchoring to (b=5 for lens 0), they will always keep this ratio. Again, the vary flag *must* be off for the parameter being anchored.

	We can still anchor the lens center to another lens the old way, but in order to distinguish from the above anchoring, now the command is "anchor\_center=...". So in the previous example, if we wanted to also anchor the center of the lens to lens 0, we do

	fit lens alpha 2.5/anchor=0,0 1 0 0.8 30 anchor\_center=0  
	1 0 0 1 1 0 0

	The vary flags for the center coordinates must be entered as zeroes, or they can be left off altogether.

2. Fit parameter limits are now assigned default values for certain parameters. For example, in any lens, when you type "fit plimits" you will see that by default, the 'q' parameters have limits from 0 to 1, and so on. This used to be done "under the hood" but now is made explicit using plimits. The plimits are also used to define ranges when plotting approximate posteriors using the Fisher matrix (with 'mkdist ... -fP').

3. You no longer have to load an input script by prefacing with '-f:' before writing the file name. Now you can simply type "qlens script.in" (or whatever you call your script).
